// Preloaded in every no-proot process. Two /proc views fakechroot gets wrong:
//  1. /proc/self/exe: programs are started as "ld-linux-aarch64.so.1 /usr/bin/prog", so the kernel's link points
//     at the loader. Programs that locate their files next to their binary (Firefox: "Couldn't load XPCOM",
//     Quickshell's relaunch...) look in the glibc dir. Answer with the real program, from argv[0].
//  2. every /proc/*/{fd/N,cwd,exe} link: the kernel answers HOST paths (/data/data/.../rootfs/usr/...). A real
//     chroot shows /usr/...; with the prefix, fakechroot prepends it again when the path is reused.
// Build (rootfs): gcc -O2 -shared -fPIC -o android-compat.so android-compat.c go-syscalls.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *program_invocation_name; // argv[0]

// Read once by the constructor: bash replaces getenv() with its own, which early in "bash --posix -i" startup
// (kitty's shell integration) does not see the environment yet.
static const char *rootfs_base;

static const char *strip_base(char *path)
{
    const char *base = rootfs_base;
    size_t bl = base ? strlen(base) : 0;
    if (bl && strncmp(path, base, bl) == 0 && path[bl] == '/')
        memmove(path, path + bl, strlen(path + bl) + 1);
    return path;
}

static const char *main_program(void)
{
    // ld.so run explicitly ("ld.so /full/path/prog args") shifts argv so argv[0] is the program path; fakechroot's
    // elfloader exec passes the full host path there. Relative argv[0] means the kernel started us: not ours.
    // The kernel's /proc/self/exe is the RESOLVED file (/usr/bin/firefox -> /usr/lib/firefox/firefox): resolve too.
    static char resolved[4096];
    if (!program_invocation_name || program_invocation_name[0] != '/')
        return NULL;
    if (!resolved[0]) {
        if (!realpath(program_invocation_name, resolved))
            snprintf(resolved, sizeof resolved, "%s", program_invocation_name);
        strip_base(resolved);
    }
    return resolved;
}

// 7. Go programs: their raw syscalls bypass every wrapper here and fakechroot's; see go-syscalls.c.
void go_syscalls_init(const char *program, const char *self_guest);

// 7c. Chromium's sandbox (Chromium, Brave, Chrome, every Electron app) needs user namespaces or a setuid helper:
//     neither exists for an Android app, and Chromium aborts rather than run without one. Instead of fixing browsers
//     one by one, recognize any Chromium-based program by what it ships next to its binary (chrome-sandbox or
//     chrome_crashpad_handler) and re-exec it once with --no-sandbox; it passes the switch on to its children.
//     Same trade-off as Firefox's sandboxes (MOZ_DISABLE_*_SANDBOX in lotfix-run).
static int has_file(const char *dir, const char *name)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static void chromium_no_sandbox(int argc, char **argv)
{
    const char *prog = main_program();
    if (!prog || !argv || strstr(prog, "crashpad") || strstr(prog, "chrome-sandbox"))
        return;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", prog);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = 0;
    if (!has_file(dir, "chrome-sandbox") && !has_file(dir, "chrome_crashpad_handler"))
        return;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--no-sandbox") == 0 || strncmp(argv[i], "--type=", 7) == 0)
            return; // already done, or a child process (the browser forwards the switch)
    char **av = calloc(argc + 2, sizeof *av);
    if (!av)
        return;
    av[0] = argv[0];
    av[1] = "--no-sandbox";
    for (int i = 1; i < argc; i++)
        av[i + 1] = argv[i];
    execv(prog, av); // through fakechroot and the loader; only returns on failure, then run as is
    free(av);
}

__attribute__((constructor)) static void android_compat_init(int argc, char **argv, char **envp)
{
    (void)envp;
    rootfs_base = getenv("FAKECHROOT_BASE");
    chromium_no_sandbox(argc, argv);
    go_syscalls_init(program_invocation_name, main_program()); // path base for every process, Go trap for Go
    if (program_invocation_name && program_invocation_name[0] == '/') {
        // fakechroot's loader exec passes the HOST path as argv[0]. Programs that locate their files from it
        // (gcc: cc1, specs...) then build host paths that fakechroot prefixes a second time. Show the guest path.
        strip_base(program_invocation_name); // in place: argv[0] is the same string
    }
    // Started through the loader, every process was named "ld-linux-aarch6" (/proc/N/comm): pkill/killall/pgrep by
    // name found nothing, so stray instances of single-instance apps survived (Dolphin then closed at once).
    const char *prog = main_program();
    if (prog && strrchr(prog, '/'))
        prctl(PR_SET_NAME, strrchr(prog, '/') + 1);
}

static int is_self_exe(const char *path)
{
    if (strcmp(path, "/proc/self/exe") == 0 || strcmp(path, "/proc/thread-self/exe") == 0)
        return 1;
    char own[64];
    snprintf(own, sizeof own, "/proc/%d/exe", getpid());
    return strcmp(path, own) == 0;
}

static ssize_t copy_out(const char *s, char *buf, size_t len)
{
    size_t n = strlen(s);
    if (n > len)
        n = len; // readlink truncates silently
    memcpy(buf, s, n);
    return (ssize_t)n;
}

// Shared logic: path is a /proc link, raw = what the kernel/next layer answered (n bytes, not NUL-terminated).
static ssize_t fix_proc_link(const char *path, char *buf, size_t len, ssize_t n)
{
    if (is_self_exe(path)) {
        const char *prog = main_program();
        if (prog)
            return copy_out(prog, buf, len);
    }
    if (n <= 0)
        return n;
    char tmp[4096];
    size_t m = (size_t)n < sizeof tmp - 1 ? (size_t)n : sizeof tmp - 1;
    memcpy(tmp, buf, m);
    tmp[m] = 0;
    return copy_out(strip_base(tmp), buf, len);
}

#define IS_PROC(p) ((p) && strncmp((p), "/proc/", 6) == 0)

ssize_t readlink(const char *path, char *buf, size_t len)
{
    static ssize_t (*next)(const char *, char *, size_t);
    if (!next)
        next = dlsym(RTLD_NEXT, "readlink");
    ssize_t n = next(path, buf, len);
    return IS_PROC(path) ? fix_proc_link(path, buf, len, n) : n;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t len)
{
    static ssize_t (*next)(int, const char *, char *, size_t);
    if (!next)
        next = dlsym(RTLD_NEXT, "readlinkat");
    ssize_t n = next(dirfd, path, buf, len);
    return IS_PROC(path) ? fix_proc_link(path, buf, len, n) : n;
}

// 3. POSIX shared memory: Android has no /dev/shm, and glibc's shm_open() opens it internally, so fakechroot cannot
//    redirect it (Firefox: MOZ_CRASH on /dev/shm/org.mozilla.ipc.*). Keep the segments in the rootfs instead.
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static int shm_path(const char *name, char *out, size_t len)
{
    if (!name || name[0] != '/' || strchr(name + 1, '/') || !name[1]) {
        errno = EINVAL;
        return -1;
    }
    mkdir("/tmp/.shm", 01777); // through fakechroot: lands in <rootfs>/tmp/.shm
    snprintf(out, len, "/tmp/.shm%s", name);
    return 0;
}

int shm_open(const char *name, int oflag, mode_t mode)
{
    char p[512];
    if (shm_path(name, p, sizeof p) < 0)
        return -1;
    return open(p, oflag | O_NOFOLLOW | O_CLOEXEC, mode);
}

int shm_unlink(const char *name)
{
    char p[512];
    if (shm_path(name, p, sizeof p) < 0)
        return -1;
    return unlink(p);
}

// 3'. Reopening a memfd through /proc/self/fd/N: SELinux denies it to apps. Firefox does it to make read-only copies
//     of its shared memory; when it failed, its parent fell back to shm_open (files on disk: slow, like proot) and its
//     content processes rejected those handles. Give back a duplicate of the memfd instead (same memory; it stays
//     writable through the copy, which a read-only reopen would not allow).
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

// 3''. Chromium (Brave, Electron) does want a genuinely read-only reopen: it checks F_GETFL == O_RDONLY and aborts
//      (brk) on the read-write duplicate. A memfd cannot provide that here, so Chromium's regions ("shared-memory-region")
//      become files in the rootfs's /tmp/.shm, reopened read-only by path. Seals (memfd-only) are pretended. The file
//      is unlinked when the original fd is closed; open copies keep the memory.
// ponytail: disk-backed (slower than RAM), Chromium's regions only; a tmpfs would avoid it but apps cannot mount.
#define CR_FDS 4096
static char *cr_paths[CR_FDS]; // fd -> backing file of a Chromium "memfd"
static unsigned cr_seals[CR_FDS];

static int cr_memfd(const char *name, unsigned int flags)
{
    static unsigned counter;
    char p[256];
    mkdir("/tmp/.shm", 01777);
    snprintf(p, sizeof p, "/tmp/.shm/cr-%d-%u-%s", (int)getpid(), __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED), name);
    int fd = open(p, O_RDWR | O_CREAT | O_EXCL | ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0), 0600);
    if (fd < 0)
        return -1;
    if (fd >= CR_FDS) {
        unlink(p); // cannot track it: plain unlinked file (no read-only reopen)
        return fd;
    }
    free(cr_paths[fd]);
    cr_paths[fd] = strdup(p);
    cr_seals[fd] = 0;
    return fd;
}

int memfd_create(const char *name, unsigned int flags)
{
    static int (*next)(const char *, unsigned int);
    if (name && strcmp(name, "shared-memory-region") == 0)
        return cr_memfd(name, flags);
    if (!next)
        next = dlsym(RTLD_NEXT, "memfd_create");
    return next(name, flags);
}

int close(int fd)
{
    static int (*next)(int);
    if (!next)
        next = dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < CR_FDS && cr_paths[fd]) {
        unlink(cr_paths[fd]);
        free(cr_paths[fd]);
        cr_paths[fd] = NULL;
    }
    return next(fd);
}

int fcntl(int fd, int cmd, ...)
{
    static int (*next)(int, int, ...);
    if (!next)
        next = dlsym(RTLD_NEXT, "fcntl");
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (fd >= 0 && fd < CR_FDS && cr_paths[fd]) {
        if (cmd == F_ADD_SEALS) { // pretend: a regular file has no seals
            cr_seals[fd] |= (unsigned)arg;
            return 0;
        }
        if (cmd == F_GET_SEALS)
            return (int)cr_seals[fd];
    }
    return next(fd, cmd, arg);
}

static int reopen_own_memfd(const char *path)
{
    const char *p = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        p = path + 14;
    else if (strncmp(path, "/proc/thread-self/fd/", 21) == 0)
        p = path + 21;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        p = path + 8;
    if (!p || !*p)
        return -1;
    char *end;
    long fd = strtol(p, &end, 10);
    if (*end || fd < 0)
        return -1;
    if (fd < CR_FDS && cr_paths[fd]) // Chromium region: a real read-only reopen through its backing file
        return open(cr_paths[fd], O_RDONLY | O_CLOEXEC);
    char link[64], target[64];
    snprintf(link, sizeof link, "/proc/self/fd/%ld", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n < 7 || strncmp(target, "/memfd:", 7) != 0)
        return -1;
    return fcntl((int)fd, F_DUPFD_CLOEXEC, 0);
}

// 3a. Denied /proc files (probed as the app: loadavg, version, net/*, sys/kernel/*...): tools exit on EACCES.
//     /proc/uptime and /proc/stat: SELinux denies them to apps; uptime, htop and systemctl (process ages from the
//     boot time) need them. Serve them from CLOCK_BOOTTIME in a memfd: uptime with idle 0, stat with a zero "cpu" line
//     and the real "btime". /proc/bus/pci/devices is denied too, and libpci exit()s on that: Firefox's gfxtest died
//     before probing EGL, so WebRender was blocklisted (GLXTEST_FAILED). Serve it empty: a tablet has no PCI bus.
//     Every other path passes through.
// ponytail: no per-CPU times in the fake /proc/stat; CPU meters (htop, top) stay at zero.
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>

// Contents for a denied /proc file, or -1 to pass the path through.
static int fake_proc(const char *p, char *buf, size_t sz)
{
    static const char *const fixed[][2] = {
        {"bus/pci/devices", ""}, // a tablet has no PCI bus
        {"sys/kernel/pid_max", "4194304\n"},
        {"sys/fs/file-max", "9223372036854775807\n"},
        {"sys/vm/overcommit_memory", "0\n"},
        {"cgroups", "#subsys_name\thierarchy\tnum_cgroups\tenabled\n"},
        {"swaps", "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"},
        {"filesystems", "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\n\text4\n\tf2fs\n\tvfat\n"},
        {"net/dev", "Inter-|   Receive                                                |  Transmit\n"
                    " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop "
                    "fifo colls carrier compressed\n"},
        {"net/route", "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"},
        {"net/arp", "IP address       HW type     Flags       HW address            Mask     Device\n"},
        {"net/tcp", "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"},
        {"net/udp", "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"},
        {"net/tcp6", "  sl  local_address remote_address st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"},
        {"net/udp6", "  sl  local_address remote_address st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"},
        {"net/if_inet6", ""}, {"partitions", "major minor  #blocks  name\n\n"}, {"diskstats", ""}, {"vmstat", ""},
        {"zoneinfo", ""}, {"cmdline", "\n"}, {"modules", ""}, {"interrupts", ""}, {"pressure/cpu", ""},
    };
    if (!p || strncmp(p, "/proc/", 6) != 0)
        return -1;
    p += 6;
    struct timespec t;
    struct sysinfo si;
    struct utsname u;
    clock_gettime(CLOCK_BOOTTIME, &t);
    if (strcmp(p, "uptime") == 0)
        return snprintf(buf, sz, "%ld.%02ld 0.00\n", (long)t.tv_sec, t.tv_nsec / 10000000);
    if (strcmp(p, "stat") == 0)
        return snprintf(buf, sz, "cpu  0 0 0 0 0 0 0 0 0 0\nbtime %ld\n", (long)(time(NULL) - t.tv_sec));
    if (strcmp(p, "loadavg") == 0 && sysinfo(&si) == 0)
        return snprintf(buf, sz, "%.2f %.2f %.2f 1/%d %d\n", si.loads[0] / 65536.0, si.loads[1] / 65536.0,
                        si.loads[2] / 65536.0, si.procs, getpid());
    if (uname(&u) == 0) {
        if (strcmp(p, "version") == 0)
            return snprintf(buf, sz, "Linux version %s (android) %s\n", u.release, u.version);
        if (strcmp(p, "sys/kernel/osrelease") == 0)
            return snprintf(buf, sz, "%s\n", u.release);
        if (strcmp(p, "sys/kernel/hostname") == 0)
            return snprintf(buf, sz, "%s\n", u.nodename);
    }
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (strcmp(p, fixed[i][0]) == 0)
            return snprintf(buf, sz, "%s", fixed[i][1]);
    return -1;
}

static int fake_proc_fd(const char *path)
{
    char buf[512];
    int n = fake_proc(path, buf, sizeof buf);
    int fd = memfd_create("proc", MFD_CLOEXEC);
    if (fd < 0 || write(fd, buf, n) != n || lseek(fd, 0, SEEK_SET) < 0) {
        if (fd >= 0)
            close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

#include <sys/syscall.h> // raw openat for the journal: our open wrappers would recurse on a denial
// After an EACCES: a reopened own memfd, or the original error. Denials go to one journal for every program
// ($XDG_RUNTIME_DIR/denied.log: "program path"), so the next missing fake is found by reading it, not per app.
static int reopen_or_fail(const char *path)
{
    int err = errno;
    // /proc/<pid>/root: xdg-desktop-portal opens it to look for /.flatpak-info and refuses every request when it
    // cannot ("Portal operation not allowed"; Android answers EACCES or ENOENT). All our processes share the rootfs:
    // answer with it (a host app).
    char *e;
    if (path && rootfs_base && strncmp(path, "/proc/", 6) == 0 && strtol(path + 6, &e, 10) > 0 &&
        strcmp(e, "/root") == 0)
        return syscall(SYS_openat, AT_FDCWD, rootfs_base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (err != EACCES) {
        errno = err;
        return -1;
    }
    int fd = path ? reopen_own_memfd(path) : -1;
    if (fd < 0 && path && strncmp(path, "/proc/", 6) == 0 && strtol(path + 6, &e, 10) > 0 && !*e)
        path = NULL; // other users' processes (pkill, ps scan them all): expected, not worth a journal line
    if (fd < 0) {
        const char *run = getenv("XDG_RUNTIME_DIR");
        char log[512], line[1024];
        if (run && path && snprintf(log, sizeof log, "%s/denied.log", run) < (int)sizeof log) {
            int l = syscall(SYS_openat, AT_FDCWD, log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
            if (l >= 0) {
                int n = snprintf(line, sizeof line, "%s %s\n", program_invocation_short_name, path);
                if (write(l, line, n > (int)sizeof line ? (int)sizeof line : n) < 0) {}
                close(l);
            }
        }
        errno = EACCES;
    }
    return fd;
}

#define IS_UPTIME(p) (fake_proc((p), (char[512]){0}, 512) >= 0)

#define OPEN_WRAP(name, ...)                                                                                          \
    int name(const char *path, int flags, ...)                                                                          \
    {                                                                                                                   \
        static int (*next)(const char *, int, ...);                                                                     \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                         \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        va_list ap;                                                                                                     \
        va_start(ap, flags);                                                                                            \
        mode_t mode = va_arg(ap, mode_t);                                                                               \
        va_end(ap);                                                                                                     \
        int r = next(path, flags, mode);                                                                                \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPEN_WRAP(open)
OPEN_WRAP(open64)

#define OPENAT_WRAP(name)                                                                                             \
    int name(int dirfd, const char *path, int flags, ...)                                                               \
    {                                                                                                                   \
        static int (*next)(int, const char *, int, ...);                                                                \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                         \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        va_list ap;                                                                                                     \
        va_start(ap, flags);                                                                                            \
        mode_t mode = va_arg(ap, mode_t);                                                                               \
        va_end(ap);                                                                                                     \
        int r = next(dirfd, path, flags, mode);                                                                         \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPENAT_WRAP(openat)
OPENAT_WRAP(openat64)

// Fortified callers (-D_FORTIFY_SOURCE, e.g. Firefox) open through these, without a mode argument.
#define OPEN2_WRAP(name)                                                                                              \
    int name(const char *path, int flags)                                                                               \
    {                                                                                                                   \
        static int (*next)(const char *, int);                                                                          \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                  \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        int r = next(path, flags);                                                                                      \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPEN2_WRAP(__open_2)
OPEN2_WRAP(__open64_2)

#define OPENAT2_WRAP(name)                                                                                            \
    int name(int dirfd, const char *path, int flags)                                                                    \
    {                                                                                                                   \
        static int (*next)(int, const char *, int);                                                                     \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                  \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        int r = next(dirfd, path, flags);                                                                               \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPENAT2_WRAP(__openat_2)
OPENAT2_WRAP(__openat64_2)

// /proc/self/maps read through stdio: programs looking for their own install dir (VLC: libvlccore's path -> plugins)
// got host paths, which fakechroot then prefixed again. Serve a copy with the rootfs prefix removed. Raw open()
// readers (sanitizers, Chromium) keep the real file.
static FILE *guest_maps(FILE *real)
{
    size_t n = rootfs_base ? strlen(rootfs_base) : 0;
    int fd = memfd_create("maps", MFD_CLOEXEC);
    if (!n || fd < 0) {
        if (fd >= 0)
            close(fd);
        return real;
    }
    char line[4096];
    while (fgets(line, sizeof line, real)) {
        char *p = strstr(line, rootfs_base);
        if (p && p[n] == '/')
            memmove(p, p + n, strlen(p + n) + 1);
        if (write(fd, line, strlen(line)) < 0)
            break;
    }
    fclose(real);
    lseek(fd, 0, SEEK_SET);
    return fdopen(fd, "r");
}

#define FOPEN_WRAP(name)                                                                                              \
    FILE *name(const char *path, const char *mode)                                                                      \
    {                                                                                                                   \
        static FILE *(*next)(const char *, const char *);                                                               \
        if (IS_UPTIME(path)) {                                                                                          \
            int fd = fake_proc_fd(path);                                                                                       \
            return fd < 0 ? NULL : fdopen(fd, "r");                                                                     \
        }                                                                                                               \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        FILE *f = next(path, mode);                                                                                     \
        return f && path && strcmp(path, "/proc/self/maps") == 0 ? guest_maps(f) : f;                                  \
    }
FOPEN_WRAP(fopen)
FOPEN_WRAP(fopen64)

// stat/access on the faked /proc files: programs test them before opening (LibreOffice: "/proc not mounted").
// Answer with the attributes of a readable /proc file.
#include <sys/stat.h>
#define STAT_WRAP(name, type)                                                                                         \
    int name(const char *path, type *st)                                                                               \
    {                                                                                                                  \
        static int (*next)(const char *, type *);                                                                     \
        if (!next)                                                                                                     \
            next = dlsym(RTLD_NEXT, #name);                                                                            \
        int r = next(path, st);                                                                                        \
        return r < 0 && errno == EACCES && IS_UPTIME(path) ? next("/proc/self/stat", st) : r;                          \
    }
STAT_WRAP(stat, struct stat)
STAT_WRAP(stat64, struct stat64)
STAT_WRAP(lstat, struct stat)
STAT_WRAP(lstat64, struct stat64)
#define FSTATAT_WRAP(name, type)                                                                                      \
    int name(int dirfd, const char *path, type *st, int flags)                                                         \
    {                                                                                                                  \
        static int (*next)(int, const char *, type *, int);                                                           \
        if (!next)                                                                                                     \
            next = dlsym(RTLD_NEXT, #name);                                                                            \
        int r = next(dirfd, path, st, flags);                                                                          \
        return r < 0 && errno == EACCES && IS_UPTIME(path) ? next(AT_FDCWD, "/proc/self/stat", st, flags) : r;         \
    }
FSTATAT_WRAP(fstatat, struct stat)
FSTATAT_WRAP(fstatat64, struct stat64)

int access(const char *path, int mode)
{
    static int (*next)(const char *, int);
    if (!next)
        next = dlsym(RTLD_NEXT, "access");
    int r = next(path, mode);
    return r < 0 && errno == EACCES && IS_UPTIME(path) && !(mode & (W_OK | X_OK)) ? 0 : r;
}

// 3b. Hard links: Android's SELinux denies them to apps (EACCES). X servers create their lock file by linking a
//     temp file into place ("Linking lock file in place failed"). Fall back to an exclusive copy: same "only one
//     creator wins" guarantee, but a separate inode (later writes to one name do not show in the other).
static int copy_excl(int olddirfd, const char *old, int newdirfd, const char *new)
{
    int in = openat(olddirfd, old, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    struct stat st;
    int out = fstat(in, &st) < 0 || !S_ISREG(st.st_mode)
                  ? (errno = EACCES, -1)
                  : openat(newdirfd, new, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        return -1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
    close(in);
    close(out);
    if (n < 0) {
        unlinkat(newdirfd, new, 0);
        return -1;
    }
    return 0;
}

int linkat(int olddirfd, const char *old, int newdirfd, const char *new, int flags)
{
    static int (*next)(int, const char *, int, const char *, int);
    if (!next)
        next = dlsym(RTLD_NEXT, "linkat");
    int r = next(olddirfd, old, newdirfd, new, flags);
    return r < 0 && (errno == EACCES || errno == EPERM) ? copy_excl(olddirfd, old, newdirfd, new) : r;
}

int link(const char *old, const char *new)
{
    return linkat(AT_FDCWD, old, AT_FDCWD, new, 0);
}

// 3c. setgid/setuid: Android's seccomp kills them even when they change nothing. Programs dropping privileges they
//     never had call them with the current ids (Xwayland before running xkbcomp: "Bad system call"). Those succeed as
//     the no-ops they are; anything else fails like an unprivileged process would.
// ponytail: only setgid/setuid seen so far; same treatment for setre*/setres* if a program needs it.
int setgid(gid_t g)
{
    if (g == getgid() && g == getegid())
        return 0;
    errno = EPERM;
    return -1;
}

int setuid(uid_t u)
{
    if (u == getuid() && u == geteuid())
        return 0;
    errno = EPERM;
    return -1;
}

// 4. accept(): Android's app seccomp filter only allows accept4 on arm64 (bionic never uses accept), and glibc has
//    no fallback for it, so every server dies with SIGSYS (fakeroot's faked, local web servers...). Same call.
#include <sys/socket.h>

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return accept4(fd, addr, len, 0);
}

// 5. syscall(NR, ...): code that issues raw syscalls through glibc's generic wrapper (Mesa uses kcmp to compare
//    fds) is out of reach of the binary patch in patch-glibc-syscalls.py. For syscalls Android's app filter kills,
//    answer ENOSYS like an old kernel would; callers already handle that.
#include <stdarg.h>
#include <sys/syscall.h>

static int android_blocked(long nr)
{
    switch (nr) {
    case SYS_kcmp:
    case SYS_set_robust_list:
    case SYS_rseq:
    case SYS_faccessat2:
    case SYS_setfsuid:
    case SYS_setfsgid:
        return 1;
    default:
        return 0;
    }
}

// Raw path syscalls bypass fakechroot (libuv's statx: neovim found no "/root"). Give them the rootfs prefix like
// fakechroot would, for every program at once: index of each path argument (-1: none).
const char *gs_translate(const char *p, char *out);

static void path_args(long nr, int *i, int *j)
{
    *i = *j = -1;
    switch (nr) {
    case SYS_openat: case SYS_openat2: case SYS_newfstatat: case SYS_faccessat: case SYS_faccessat2:
    case SYS_mkdirat: case SYS_mknodat: case SYS_unlinkat: case SYS_fchmodat: case SYS_fchownat:
    case SYS_statx: case SYS_utimensat: case SYS_readlinkat: case SYS_inotify_add_watch:
    case SYS_getxattr: case SYS_lgetxattr: case SYS_setxattr: case SYS_lsetxattr: case SYS_listxattr:
    case SYS_llistxattr: case SYS_removexattr: case SYS_lremovexattr: case SYS_name_to_handle_at:
        *i = (nr >= SYS_setxattr && nr <= SYS_lremovexattr) ? 0 : 1;
        break;
    case SYS_truncate: case SYS_statfs: case SYS_chdir: case SYS_chroot: case SYS_execve: case SYS_execveat:
        *i = nr == SYS_execveat ? 1 : 0;
        break;
    case SYS_symlinkat:
        *i = 2;
        break;
    case SYS_linkat: case SYS_renameat: case SYS_renameat2:
        *i = 1, *j = 3;
        break;
    }
}

long syscall(long nr, ...)
{
    static long (*next)(long, ...);
    va_list ap;
    long a[6];
    char p1[4096], p2[4096];
    int i, j;
    va_start(ap, nr);
    for (int k = 0; k < 6; k++)
        a[k] = va_arg(ap, long);
    va_end(ap);
    if (android_blocked(nr)) {
        errno = ENOSYS;
        return -1;
    }
    if (nr == SYS_memfd_create) // Chromium calls it raw: same routing as the libc wrapper (section 3'')
        return memfd_create((const char *)a[0], (unsigned int)a[1]);
    path_args(nr, &i, &j);
    if (i >= 0)
        a[i] = (long)gs_translate((const char *)a[i], p1);
    if (j >= 0)
        a[j] = (long)gs_translate((const char *)a[j], p2);
    if (!next)
        next = dlsym(RTLD_NEXT, "syscall");
    return next(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}

// 6. tcgetattr/tcsetattr: recent glibc uses the TCGETS2/TCSETS2 ioctls, which Android's SELinux ioctl allowlist
//    refuses on ptys (EACCES): foot, shells, editors fail. Use the classic TCGETS/TCSETS + the kernel's struct.
#include <sys/ioctl.h>
#include <termios.h>

struct kernel_termios { // asm-generic struct termios (no speed fields)
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t     c_line;
    cc_t     c_cc[19];
};

int tcgetattr(int fd, struct termios *t)
{
    struct kernel_termios k;
    if (ioctl(fd, TCGETS, &k) < 0)
        return -1;
    memset(t, 0, sizeof *t);
    t->c_iflag = k.c_iflag;
    t->c_oflag = k.c_oflag;
    t->c_cflag = k.c_cflag;
    t->c_lflag = k.c_lflag;
    t->c_line  = k.c_line;
    memcpy(t->c_cc, k.c_cc, sizeof k.c_cc < sizeof t->c_cc ? sizeof k.c_cc : sizeof t->c_cc);
    cfsetspeed(t, k.c_cflag & CBAUD); // let glibc fill its speed fields from the encoded baud bits
    t->c_cflag = k.c_cflag;
    return 0;
}

int tcsetattr(int fd, int act, const struct termios *t)
{
    static const unsigned long cmd[] = {TCSETS, TCSETSW, TCSETSF}; // TCSANOW, TCSADRAIN, TCSAFLUSH
    if (act < 0 || act > 2) {
        errno = EINVAL;
        return -1;
    }
    struct kernel_termios k = {t->c_iflag, t->c_oflag, t->c_cflag, t->c_lflag, t->c_line, {0}};
    memcpy(k.c_cc, t->c_cc, sizeof k.c_cc);
    return ioctl(fd, cmd[act], &k);
}

// glibc's isatty/ttyname call tcgetattr INTERNALLY (TCGETS2 again), so they say "not a tty" on a real pty.
int isatty(int fd)
{
    struct kernel_termios k;
    return ioctl(fd, TCGETS, &k) == 0;
}

int ttyname_r(int fd, char *buf, size_t len)
{
    if (!isatty(fd))
        return errno == EBADF ? EBADF : ENOTTY;
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, buf, len - 1);
    if (n < 0)
        return errno;
    buf[n] = 0;
    return 0;
}

char *ttyname(int fd)
{
    static char name[256];
    int err = ttyname_r(fd, name, sizeof name);
    if (err) {
        errno = err;
        return NULL;
    }
    return name;
}

// 7b. exec of /proc/self/exe: Chromium (Brave...) starts its GPU, network and renderer processes by exec'ing
//     /proc/self/exe. The kernel's /proc/self/exe is the glibc loader that runs every program here, so the child was
//     the bare loader ("ld-linux-aarch64.so.1: loader cannot load itself"). Exec the real program instead; fakechroot's
//     execve then goes through the loader as usual.
extern char **environ;

static const char *self_exe_target(const char *path)
{
    if (path && (strcmp(path, "/proc/self/exe") == 0 || strcmp(path, "/proc/thread-self/exe") == 0)) {
        const char *p = main_program();
        if (p)
            return p;
    }
    return path;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    static int (*next)(const char *, char *const[], char *const[]);
    if (!next)
        next = dlsym(RTLD_NEXT, "execve");
    return next(self_exe_target(path), argv, envp);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

// 8. execvpe: fakechroot 2.20.1 does not wrap it, so glibc's version issues the execve syscall itself with the guest
//    path, on Android's filesystem (foot: "bash: failed to execute", or Android's /bin/sh). Search PATH here and go
//    through execve, which fakechroot wraps (loader + #! handling).

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/'))
        return execve(file, argv, envp);
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin";
    char buf[4096];
    int saw_eacces = 0;
    for (const char *p = path; *p;) {
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n + 1 + strlen(file) < sizeof buf) {
            memcpy(buf, p, n);
            buf[n] = '/';
            strcpy(buf + n + 1, file);
            execve(buf, argv, envp); // returns only on failure
            if (errno == EACCES)
                saw_eacces = 1;
        }
        if (!e)
            break;
        p = e + 1;
    }
    errno = saw_eacces ? EACCES : ENOENT;
    return -1;
}

// 8b. posix_spawn(p): glibc's clones and calls execve itself, on Android's filesystem and without the loader
//     (gcc: "cannot execute 'cc1': posix_spawnp: No such file or directory"). Same semantics with fork + the
//     execve/execvpe above, exec errors reported back through a close-on-exec pipe like glibc does.
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

// glibc's private layout of posix_spawn_file_actions_t entries (posix/spawn_int.h, stable since 2.29).
// ponytail: tied to glibc internals; recheck spawn_int.h if a glibc upgrade breaks spawned redirections.
struct spawn_action {
    enum { do_close, do_dup2, do_open, do_chdir, do_fchdir, do_closefrom, do_tcsetpgrp } tag;
    union {
        struct { int fd; } close_action;
        struct { int fd, newfd; } dup2_action;
        struct { int fd; char *path; int oflag; mode_t mode; } open_action;
        struct { char *path; } chdir_action;
        struct { int fd; } fchdir_action;
        struct { int from; } closefrom_action;
        struct { int fd; } setpgrp_action;
    } action;
};
struct spawn_file_actions {
    int allocated, used;
    struct spawn_action *actions;
};

static int apply_file_actions(const posix_spawn_file_actions_t *fa)
{
    const struct spawn_file_actions *f = (const void *)fa;
    for (int i = 0; f && i < f->used; i++) {
        const struct spawn_action *a = &f->actions[i];
        switch (a->tag) {
        case do_close:
            close(a->action.close_action.fd); // EBADF is not an error for posix_spawn
            break;
        case do_dup2:
            if (a->action.dup2_action.fd == a->action.dup2_action.newfd) {
                int fl = fcntl(a->action.dup2_action.fd, F_GETFD); // same fd: just keep it across exec
                if (fl < 0 || fcntl(a->action.dup2_action.fd, F_SETFD, fl & ~FD_CLOEXEC) < 0)
                    return -1;
            } else if (dup2(a->action.dup2_action.fd, a->action.dup2_action.newfd) < 0)
                return -1;
            break;
        case do_open: {
            int fd = open(a->action.open_action.path, a->action.open_action.oflag, a->action.open_action.mode);
            if (fd < 0)
                return -1;
            if (fd != a->action.open_action.fd) {
                if (dup2(fd, a->action.open_action.fd) < 0)
                    return -1;
                close(fd);
            }
            break;
        }
        case do_chdir:
            if (chdir(a->action.chdir_action.path) < 0)
                return -1;
            break;
        case do_fchdir:
            if (fchdir(a->action.fchdir_action.fd) < 0)
                return -1;
            break;
        case do_closefrom:
            closefrom(a->action.closefrom_action.from);
            break;
        case do_tcsetpgrp:
            if (tcsetpgrp(a->action.setpgrp_action.fd, getpgrp()) < 0)
                return -1;
            break;
        }
    }
    return 0;
}

static int apply_attrs(const posix_spawnattr_t *at)
{
    short fl = 0;
    if (!at || posix_spawnattr_getflags(at, &fl))
        return 0;
    if (fl & POSIX_SPAWN_SETSID && setsid() < 0)
        return -1;
    if (fl & POSIX_SPAWN_SETPGROUP) {
        pid_t pg = 0;
        posix_spawnattr_getpgroup(at, &pg);
        if (setpgid(0, pg) < 0)
            return -1;
    }
    if (fl & POSIX_SPAWN_RESETIDS && (setgid(getgid()) < 0 || setuid(getuid()) < 0))
        return -1;
    if (fl & POSIX_SPAWN_SETSIGDEF) {
        sigset_t def;
        posix_spawnattr_getsigdefault(at, &def);
        for (int s = 1; s < NSIG; s++)
            if (sigismember(&def, s) == 1)
                signal(s, SIG_DFL);
    }
    if (fl & POSIX_SPAWN_SETSIGMASK) {
        sigset_t mask;
        posix_spawnattr_getsigmask(at, &mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);
    } // ponytail: SETSCHEDPARAM/SETSCHEDULER ignored; add sched_setscheduler if something needs them
    return 0;
}

static int spawn(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[], int search)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return errno;
    pid_t c = fork();
    if (c < 0) {
        int e = errno;
        close(p[0]);
        close(p[1]);
        return e;
    }
    if (c == 0) {
        close(p[0]);
        if (apply_attrs(at) == 0 && apply_file_actions(fa) == 0) {
            if (search)
                execvpe(file, argv, envp ? envp : environ);
            else
                execve(file, argv, envp ? envp : environ);
        }
        int e = errno;
        (void)!write(p[1], &e, sizeof e);
        _exit(127);
    }
    close(p[1]);
    int e = 0;
    ssize_t n;
    while ((n = read(p[0], &e, sizeof e)) < 0 && errno == EINTR)
        ;
    close(p[0]);
    if (n == sizeof e) { // the child failed before exec: reap it, report its errno
        waitpid(c, NULL, 0);
        return e;
    }
    if (pid)
        *pid = c;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[])
{
    return spawn(pid, path, fa, at, argv, envp, 0);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[])
{
    return spawn(pid, file, fa, at, argv, envp, 1);
}

// 9. Users and groups: glibc's NSS "files" module opens /etc/passwd and /etc/group internally, so it reads Android's
//    (missing) files: "I have no name!", ls/whoami/id without names. Look them up in the rootfs ones instead,
//    with fopen (rewritten by fakechroot) + the standard fgetpwent_r/fgetgrent_r parsers.
#include <grp.h>
#include <pwd.h>

// Raw openat on the host path: fakechroot finds no FAKECHROOT_BASE through bash's getenv early in "bash --posix -i"
// (it then opened Android's own /etc/passwd), and it would rewrite an already-prefixed path a second time.
static FILE *open_rootfs(const char *path)
{
    char full[4096];
    snprintf(full, sizeof full, "%s%s", rootfs_base ? rootfs_base : "", path);
    int fd = syscall(SYS_openat, AT_FDCWD, full, O_RDONLY | O_CLOEXEC);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "r");
    if (fd >= 0 && !f)
        close(fd);
    return f;
}

#define LOOKUP_R(type, ent, getent, field_match)                                                                      \
    FILE *f = open_rootfs(ent);                                                                                         \
    *result = NULL;                                                                                                     \
    if (!f)                                                                                                             \
        return errno;                                                                                                   \
    int err;                                                                                                            \
    struct type *r;                                                                                                     \
    while ((err = getent(f, out, buf, len, &r)) == 0)                                                                   \
        if (field_match) {                                                                                              \
            *result = r;                                                                                                \
            break;                                                                                                      \
        }                                                                                                               \
    fclose(f);                                                                                                          \
    return err == ENOENT ? 0 : err;

int getpwuid_r(uid_t uid, struct passwd *out, char *buf, size_t len, struct passwd **result)
{
    LOOKUP_R(passwd, "/etc/passwd", fgetpwent_r, r->pw_uid == uid)
}

int getpwnam_r(const char *name, struct passwd *out, char *buf, size_t len, struct passwd **result)
{
    LOOKUP_R(passwd, "/etc/passwd", fgetpwent_r, strcmp(r->pw_name, name) == 0)
}

int getgrgid_r(gid_t gid, struct group *out, char *buf, size_t len, struct group **result)
{
    LOOKUP_R(group, "/etc/group", fgetgrent_r, r->gr_gid == gid)
}

int getgrnam_r(const char *name, struct group *out, char *buf, size_t len, struct group **result)
{
    LOOKUP_R(group, "/etc/group", fgetgrent_r, strcmp(r->gr_name, name) == 0)
}

// Non-reentrant versions on top of the _r ones (static storage, as in glibc).
#define LOOKUP(type, call)                                                                                              \
    static struct type ent;                                                                                             \
    static char buf[16384];                                                                                             \
    struct type *res = NULL;                                                                                            \
    int err = call;                                                                                                     \
    if (err)                                                                                                            \
        errno = err;                                                                                                    \
    return res;

struct passwd *getpwuid(uid_t uid) { LOOKUP(passwd, getpwuid_r(uid, &ent, buf, sizeof buf, &res)) }
struct passwd *getpwnam(const char *name) { LOOKUP(passwd, getpwnam_r(name, &ent, buf, sizeof buf, &res)) }
struct group *getgrgid(gid_t gid) { LOOKUP(group, getgrgid_r(gid, &ent, buf, sizeof buf, &res)) }
struct group *getgrnam(const char *name) { LOOKUP(group, getgrnam_r(name, &ent, buf, sizeof buf, &res)) }

// 9. DNS: glibc reads /etc/resolv.conf internally (Android's /etc: none), so every lookup went to 127.0.0.1.
//    glibc honours nameservers an application sets in _res after res_init, and _res is per thread: before each
//    lookup, give the calling thread the rootfs's nameservers. (Go programs read the file via go-syscalls.)
#include <arpa/inet.h>
#include <netdb.h>
#include <resolv.h>

static void res_from_rootfs(void)
{
    static __thread int done;
    if (done)
        return;
    done = 1;
    struct __res_state *rs = __res_state();
    if (!(rs->options & RES_INIT))
        res_ninit(rs);
    FILE *f = open_rootfs("/etc/resolv.conf");
    if (!f)
        return;
    char line[256], ip[64];
    int n = 0;
    while (n < MAXNS && fgets(line, sizeof line, f))
        if (sscanf(line, " nameserver %63s", ip) == 1 && inet_pton(AF_INET, ip, &rs->nsaddr_list[n].sin_addr) == 1) {
            rs->nsaddr_list[n].sin_family = AF_INET;
            rs->nsaddr_list[n].sin_port = htons(53);
            n++;
        } // ponytail: IPv4 nameservers only; add _u._ext.nsaddrs for IPv6 ones if ever needed
    fclose(f);
    if (n)
        rs->nscount = n;
}

#define RESOLVE(ret, name, params, args)                                                                              \
    ret name params                                                                                                     \
    {                                                                                                                   \
        static ret(*next) params;                                                                                       \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        res_from_rootfs();                                                                                              \
        return next args;                                                                                               \
    }

RESOLVE(int, getaddrinfo, (const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res),
        (node, service, hints, res))
RESOLVE(struct hostent *, gethostbyname, (const char *name), (name))
RESOLVE(struct hostent *, gethostbyname2, (const char *name, int af), (name, af))
RESOLVE(int, gethostbyname_r,
        (const char *name, struct hostent *ret, char *buf, size_t len, struct hostent **result, int *err),
        (name, ret, buf, len, result, err))
RESOLVE(int, gethostbyname2_r,
        (const char *name, int af, struct hostent *ret, char *buf, size_t len, struct hostent **result, int *err),
        (name, af, ret, buf, len, result, err))

// Fortified callers (-D_FORTIFY_SOURCE, e.g. Firefox) call these instead.
ssize_t __readlink_chk(const char *path, char *buf, size_t len, size_t buflen)
{
    static ssize_t (*next)(const char *, char *, size_t, size_t);
    if (!next)
        next = dlsym(RTLD_NEXT, "__readlink_chk");
    ssize_t n = next(path, buf, len, buflen);
    return IS_PROC(path) ? fix_proc_link(path, buf, len < buflen ? len : buflen, n) : n;
}

ssize_t __readlinkat_chk(int dirfd, const char *path, char *buf, size_t len, size_t buflen)
{
    static ssize_t (*next)(int, const char *, char *, size_t, size_t);
    if (!next)
        next = dlsym(RTLD_NEXT, "__readlinkat_chk");
    ssize_t n = next(dirfd, path, buf, len, buflen);
    return IS_PROC(path) ? fix_proc_link(path, buf, len < buflen ? len : buflen, n) : n;
}

// dladdr: the loader knows libraries by their host path (fakechroot loads them from the rootfs). Programs that find
// their data next to their own library (VLC's plugins, relocatable builds) then got the rootfs prefixed twice.
// Show the guest path: the suffix of the same string, so it stays valid as long as the library.
static void guest_fname(Dl_info *info)
{
    size_t n = rootfs_base ? strlen(rootfs_base) : 0;
    if (n && info->dli_fname && strncmp(info->dli_fname, rootfs_base, n) == 0 && info->dli_fname[n] == '/')
        info->dli_fname += n;
}

int dladdr(const void *addr, Dl_info *info)
{
    static int (*next)(const void *, Dl_info *);
    if (!next)
        next = dlsym(RTLD_NEXT, "dladdr");
    int r = next(addr, info);
    if (r)
        guest_fname(info);
    return r;
}

int dladdr1(const void *addr, Dl_info *info, void **extra, int flags)
{
    static int (*next)(const void *, Dl_info *, void **, int);
    if (!next)
        next = dlsym(RTLD_NEXT, "dladdr1");
    int r = next(addr, info, extra, flags);
    if (r)
        guest_fname(info);
    return r;
}

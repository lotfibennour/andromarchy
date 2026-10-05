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
//     Also --render-node-override=/dev/kgsl-3d0: no DRM render node for apps, so Chromium composited in software;
//     Mesa's GBM works on the KGSL device (DRM_IOCTL_VERSION answered by the ioctl wrapper below): GPU compositing.
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
    if (getenv("ELECTRON_RUN_AS_NODE")) // Electron as plain Node (VS Code's CLI): no Chromium, rejects the flag
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
    int kgsl = access("/dev/kgsl-3d0", R_OK | W_OK) == 0;
    for (int i = 1; i < argc && kgsl; i++)
        if (strncmp(argv[i], "--render-node-override", 22) == 0)
            kgsl = 0; // the user's own choice
    // Hardware video (VA-API -> Mesa's termuxva bridge -> termux-va/MediaCodec): Chromium opens libva on the KGSL
    // node, which libdrm only accepts through Mesa's DRM shim (LD_PRELOAD, inherited by the GPU process).
    char shim[4096];
    snprintf(shim, sizeof shim, "%s/usr/lib/libtva_drm_shim_wayland.so", rootfs_base ? rootfs_base : "");
    // ponytail: opt-in (ANDROMARCHY_CHROMIUM_VAAPI=1) until it plays cleanly: YouTube in Brave showed green frames
    // and stalls even at 720p (copies per frame + frames late), while ffmpeg/mpv decode fine through the same bridge.
    int va = kgsl && rootfs_base && getenv("ANDROMARCHY_CHROMIUM_VAAPI") && getenv("LIBVA_DRIVER_NAME") &&
             access("/usr/lib/libtva_drm_shim_wayland.so", R_OK) == 0; // guest path: fakechroot
    if (va) {
        const char *pre = getenv("LD_PRELOAD");
        char *np;
        if (asprintf(&np, "%s%s%s", pre ? pre : "", pre ? ":" : "", shim) >= 0)
            setenv("LD_PRELOAD", np, 1);
    }
    char **av = calloc(argc + 6, sizeof *av);
    if (!av)
        return;
    int n = 0;
    av[n++] = argv[0];
    av[n++] = "--no-sandbox";
    // Cameras are PipeWire nodes here (the Anland backend's Android cameras; no /dev/video*): the portal path.
    // (one --enable-features: Chromium keeps only the last one)
    av[n++] = va ? "--enable-features=WebRtcPipeWireCamera,AcceleratedVideoDecodeLinuxGL,"
                   "AcceleratedVideoDecodeLinuxZeroCopyGL,VaapiVideoDecoder,VaapiIgnoreDriverChecks"
                 : "--enable-features=WebRtcPipeWireCamera";
    if (kgsl) {
        av[n++] = "--render-node-override=/dev/kgsl-3d0";
        // Decoded video in NV12 GBM buffers on KGSL showed green stripes (a row layout Chromium does not expect):
        // video frames go to ordinary textures, everything else stays on the GPU path.
        av[n++] = "--disable-gpu-memory-buffer-video-frames";
    }
    for (int i = 1; i < argc; i++)
        av[n++] = argv[i];
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

// 3'''. O_TMPFILE: an anonymous file that linkat("/proc/self/fd/N") later gives a name (libostree writes every object
//       this way: Flatpak installs; glnx, systemd, GLib's atomic writes too). Android's SELinux refuses that linkat
//       (EACCES) and cannot re-open the file through /proc, so a copy fallback has nothing to read. Emulate: a hidden
//       real file in the same directory; linkat on its fd renames it into place (RENAME_NOREPLACE: link's EEXIST
//       semantics), close() removes it if it was never linked.
// ponytail: a crash leaves a ".tmpfile-<pid>-<n>" behind; dup()ed fds are not followed; only the creating process
//           removes it (a forked child closing its copy of the fd must not delete the parent's file).
#include <sys/syscall.h>
// Debug aid: ANDROID_COMPAT_TRACE_OPEN=1 prints every failing open/openat ("open dirfd path flags -> result") on
// stderr; any other value is a path fragment whose opens are printed whatever their result.
static void trace_open(const char *fn, int dirfd, const char *path, int flags, int r)
{
    static const char *want;
    static int init;
    if (!init) {
        want = getenv("ANDROID_COMPAT_TRACE_OPEN");
        init = 1;
    }
    if (!want)
        return;
    int all = strcmp(want, "1") != 0;
    if (all ? !(path && strstr(path, want)) : r >= 0)
        return;
    int e = errno;
    char m[1200];
    int n = snprintf(m, sizeof m, "[trace] pid=%d %s dirfd=%d path=%s flags=0x%x -> %d (errno %d)\n", (int)getpid(), fn,
                     dirfd, path ? path : "(null)", flags, r, r < 0 ? e : 0);
    if (write(2, m, n) < 0) {}
    errno = e;
}

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
static struct {
    char *path;
    pid_t pid;
} tmpfiles[CR_FDS];
static int guest_dir_of(int dirfd, const char *linkpath, char *out, size_t len); // defined with symlinks (3d)

static int tmpfile_open(int dirfd, const char *dir, int flags, mode_t mode)
{
    static unsigned counter;
    char probe[4096], gdir[4096], name[4200];
    snprintf(probe, sizeof probe, "%s/x", dir && *dir ? dir : ".");
    if (guest_dir_of(dirfd, probe, gdir, sizeof gdir) < 0)
        return -1;
    snprintf(name, sizeof name, "%s/.tmpfile-%d-%u", gdir, (int)getpid(), __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED));
    int fd = open(name, (flags & ~O_TMPFILE) | O_CREAT | O_EXCL, mode);
    if (fd < 0)
        return -1;
    if (fd >= CR_FDS) {
        unlink(name); // cannot track it: a plain anonymous file (cannot be linked later)
        return fd;
    }
    free(tmpfiles[fd].path);
    tmpfiles[fd].path = strdup(name);
    tmpfiles[fd].pid = getpid();
    return fd;
}

// linkat() of an emulated O_TMPFILE: returns 1 when handled (result in *r, errno set on failure).
static int tmpfile_link(int olddirfd, const char *old, int newdirfd, const char *new, int flags, int *r)
{
    int fd = -1;
    if (old && strncmp(old, "/proc/self/fd/", 14) == 0 && old[14] >= '0' && old[14] <= '9')
        fd = atoi(old + 14);
    else if ((flags & AT_EMPTY_PATH) && old && !*old)
        fd = olddirfd;
    if (fd < 0 || fd >= CR_FDS || !tmpfiles[fd].path || tmpfiles[fd].pid != getpid())
        return 0;
    *r = syscall(SYS_renameat2, AT_FDCWD, tmpfiles[fd].path, newdirfd, new, RENAME_NOREPLACE) == 0 ? 0 : -1;
    if (*r == 0) {
        free(tmpfiles[fd].path);
        tmpfiles[fd].path = NULL;
    }
    return 1;
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
    if (fd >= 0 && fd < CR_FDS && tmpfiles[fd].path && tmpfiles[fd].pid == getpid()) {
        unlink(tmpfiles[fd].path); // never linked: anonymous, as O_TMPFILE promises
        free(tmpfiles[fd].path);
        tmpfiles[fd].path = NULL;
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

// /sys files a desktop reads that apps are denied or that Android lacks: the NUMA node list, and the machine's
// identity (DMI: fastfetch, "About" panels, hardware databases) from Android's Build values, exported by the app.
static int fake_sys(const char *p, char *buf, size_t sz)
{
    if (strcmp(p, "devices/system/node/online") == 0)
        return snprintf(buf, sz, "0\n");
    const char *dmi = strncmp(p, "class/dmi/id/", 13) == 0         ? p + 13
                      : strncmp(p, "devices/virtual/dmi/id/", 23) == 0 ? p + 23
                                                                     : NULL;
    const char *vendor = getenv("ANDROMARCHY_VENDOR"), *model = getenv("ANDROMARCHY_MODEL");
    if (!dmi || !vendor || !model)
        return -1;
    if (!strcmp(dmi, "sys_vendor") || !strcmp(dmi, "board_vendor") || !strcmp(dmi, "chassis_vendor"))
        return snprintf(buf, sz, "%s\n", vendor);
    if (!strcmp(dmi, "product_name") || !strcmp(dmi, "board_name") || !strcmp(dmi, "product_family"))
        return snprintf(buf, sz, "%s\n", model);
    if (!strcmp(dmi, "chassis_type"))
        return snprintf(buf, sz, "30\n"); // SMBIOS: tablet
    return -1;
}

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
        {"zoneinfo", ""}, {"cmdline", "\n"}, {"modules", ""}, {"interrupts", ""},
        // Kernel limits programs size themselves from (tools/paths-probe.py: denied to apps): stock Linux values.
        {"sys/kernel/threads-max", "126000\n"}, {"sys/kernel/cap_last_cap", "40\n"}, {"sys/kernel/ngroups_max", "65536\n"},
        {"sys/kernel/yama/ptrace_scope", "0\n"}, {"sys/vm/max_map_count", "65530\n"}, {"sys/vm/swappiness", "60\n"},
        {"sys/fs/nr_open", "1048576\n"}, {"sys/fs/inotify/max_user_watches", "65536\n"},
        {"sys/fs/inotify/max_user_instances", "128\n"}, {"sys/net/core/somaxconn", "4096\n"},
        {"sys/net/ipv4/ip_local_port_range", "32768\t60999\n"}, {"sys/net/ipv6/conf/all/disable_ipv6", "0\n"},
        {"pressure/cpu", "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"},
        {"pressure/memory", "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"},
        {"pressure/io", "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"},
        {"buddyinfo", ""}, {"crypto", ""}, {"misc", ""}, {"net/snmp", ""},
        {"devices", "Character devices:\n  1 mem\n  5 /dev/tty\n  5 /dev/ptmx\n136 pts\n\nBlock devices:\n"},
        {"net/unix", "Num       RefCount Protocol Flags    Type St Inode Path\n"},
        {"net/wireless", "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
                         " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"},
    };
    if (p && strncmp(p, "/sys/", 5) == 0)
        return fake_sys(p + 5, buf, sz);
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

// /dev/shm: absent on Android (and /dev stays a host path). Chromium/Electron create shared memory files there
// (VS Code, Chromium: "Creating shared memory in /dev/shm failed", then a trap). Same place as shm_open: the
// rootfs's /tmp/.shm.
static const char *dev_shm(const char *path, char *buf)
{
    // Also /sys/class/backlight (denied to apps): the app keeps an "android" backlight device in
    // $XDG_RUNTIME_DIR/backlight and applies its brightness file to its window (Omarchy's brightness keys and monitor
    // panel, brightnessctl, all read and write that directory as on a laptop).
    static const char bl[] = "/sys/class/backlight";
    const char *run = getenv("XDG_RUNTIME_DIR");
    if (path && run && strncmp(path, bl, sizeof bl - 1) == 0 && (path[sizeof bl - 1] == '/' || !path[sizeof bl - 1])) {
        snprintf(buf, 512, "%s/backlight%s", run, path + sizeof bl - 1);
        return buf;
    }
    if (!path || strncmp(path, "/dev/shm", 8) != 0 || (path[8] != '/' && path[8]))
        return path;
    mkdir("/tmp/.shm", 01777); // through fakechroot: lands in <rootfs>/tmp/.shm
    snprintf(buf, 512, "/tmp/.shm%s", path + 8);
    return buf;
}

#include <sys/statfs.h>
#include <sys/statvfs.h>
#define SHM_PATH_WRAP(ret, name, type)                                                                                \
    ret name(const char *path, type arg)                                                                               \
    {                                                                                                                  \
        static ret (*next)(const char *, type);                                                                       \
        char shm_[512];                                                                                                \
        if (!next)                                                                                                     \
            next = dlsym(RTLD_NEXT, #name);                                                                            \
        return next(dev_shm(path, shm_), arg);                                                                         \
    }
SHM_PATH_WRAP(int, statfs, struct statfs *)
SHM_PATH_WRAP(int, statfs64, struct statfs64 *)
SHM_PATH_WRAP(int, statvfs, struct statvfs *)
SHM_PATH_WRAP(int, statvfs64, struct statvfs64 *)

#include <dirent.h>
DIR *opendir(const char *path) // glibc opens the directory internally (no open() wrapper): /sys/class/backlight
{
    static DIR *(*next)(const char *);
    char shm_[512];
    if (!next)
        next = dlsym(RTLD_NEXT, "opendir");
    return next(dev_shm(path, shm_));
}

int unlink(const char *path)
{
    static int (*next)(const char *);
    char shm_[512];
    if (!next)
        next = dlsym(RTLD_NEXT, "unlink");
    return next(dev_shm(path, shm_));
}


// mkstemp family: Chromium names its /dev/shm files through mkostemp, which opens inside glibc (no open() wrapper).
// The generated name is copied back into the caller's /dev/shm template.
#define MKSTEMP_WRAP(name, params, args)                                                                              \
    int name params                                                                                                    \
    {                                                                                                                  \
        static int(*next) params;                                                                                      \
        if (!next)                                                                                                     \
            next = dlsym(RTLD_NEXT, #name);                                                                            \
        char shm_[512];                                                                                                \
        const char *orig = tmpl;                                                                                       \
        if (dev_shm(orig, shm_) == orig)                                                                               \
            return next args;                                                                                          \
        tmpl = shm_;                                                                                                   \
        int fd = next args;                                                                                            \
        if (fd >= 0)                                                                                                   \
            strcpy((char *)orig + 8, shm_ + 9); /* "/tmp/.shm" + rest -> "/dev/shm" + rest */                          \
        return fd;                                                                                                     \
    }
MKSTEMP_WRAP(mkstemp, (char *tmpl), (tmpl))
MKSTEMP_WRAP(mkstemp64, (char *tmpl), (tmpl))
MKSTEMP_WRAP(mkostemp, (char *tmpl, int flags), (tmpl, flags))
MKSTEMP_WRAP(mkostemp64, (char *tmpl, int flags), (tmpl, flags))
#define OPEN_WRAP(name, ...)                                                                                          \
    int name(const char *path, int flags, ...)                                                                          \
    {                                                                                                                   \
        static int (*next)(const char *, int, ...);                                                                     \
        char shm_[512];                                                                                                 \
        path = dev_shm(path, shm_);                                                                                     \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                         \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        va_list ap;                                                                                                     \
        va_start(ap, flags);                                                                                            \
        mode_t mode = va_arg(ap, mode_t);                                                                               \
        va_end(ap);                                                                                                     \
        if ((flags & O_TMPFILE) == O_TMPFILE)                                                                           \
            return tmpfile_open(AT_FDCWD, path, flags, mode);                                                           \
        int r = next(path, flags, mode);                                                                                \
        trace_open(#name, AT_FDCWD, path, flags, r);                                                                    \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPEN_WRAP(open)
OPEN_WRAP(open64)

#define OPENAT_WRAP(name)                                                                                             \
    int name(int dirfd, const char *path, int flags, ...)                                                               \
    {                                                                                                                   \
        static int (*next)(int, const char *, int, ...);                                                                \
        char shm_[512];                                                                                                 \
        path = dev_shm(path, shm_);                                                                                     \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                         \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        va_list ap;                                                                                                     \
        va_start(ap, flags);                                                                                            \
        mode_t mode = va_arg(ap, mode_t);                                                                               \
        va_end(ap);                                                                                                     \
        if ((flags & O_TMPFILE) == O_TMPFILE)                                                                           \
            return tmpfile_open(dirfd, path, flags, mode);                                                              \
        int r = next(dirfd, path, flags, mode);                                                                         \
        trace_open(#name, dirfd, path, flags, r);                                                                       \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPENAT_WRAP(openat)
OPENAT_WRAP(openat64)

// Fortified callers (-D_FORTIFY_SOURCE, e.g. Firefox) open through these, without a mode argument.
#define OPEN2_WRAP(name)                                                                                              \
    int name(const char *path, int flags)                                                                               \
    {                                                                                                                   \
        static int (*next)(const char *, int);                                                                          \
        char shm_[512];                                                                                                 \
        path = dev_shm(path, shm_);                                                                                     \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                  \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        int r = next(path, flags);                                                                                      \
        trace_open(#name, AT_FDCWD, path, flags, r);                                                                    \
        return r < 0 && (errno == EACCES || errno == ENOENT) ? reopen_or_fail(path) : r;                                                     \
    }
OPEN2_WRAP(__open_2)
OPEN2_WRAP(__open64_2)

#define OPENAT2_WRAP(name)                                                                                            \
    int name(int dirfd, const char *path, int flags)                                                                    \
    {                                                                                                                   \
        static int (*next)(int, const char *, int);                                                                     \
        char shm_[512];                                                                                                 \
        path = dev_shm(path, shm_);                                                                                     \
        if (IS_UPTIME(path))                                                                                            \
            return fake_proc_fd(path);                                                                                  \
        if (!next)                                                                                                      \
            next = dlsym(RTLD_NEXT, #name);                                                                             \
        int r = next(dirfd, path, flags);                                                                               \
        trace_open(#name, dirfd, path, flags, r);                                                                       \
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
        char shm_[512];                                                                                                 \
        path = dev_shm(path, shm_);                                                                                     \
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

// Hard-link fallbacks (section 3b): stat reports 1 + the copies made from a file that still exist, like a real
// link count. shadow's lock (useradd, usermod...) links a temp file to passwd.lock and checks it went to 2;
// gnome-keyring links its keyring before each save and checks it went up by one (a stuck "2" aborted it).
// ponytail: last 16 links remembered; a copy removed since no longer counts (checked with a raw stat).
static char linked_copies[16][2][512];
// Under fake root (sudo, ARCH_ROOT=1), libfakeroot answers stat from faked's database, keyed by inode. A file deleted
// outside fakeroot leaves its record, and a new directory reusing that inode came back as a regular file (git:
// "unable to unlink ...: Is a directory"). fakeroot owns owners and permissions; the file TYPE is the kernel's.
#include <sys/syscall.h>
static mode_t real_type(int dirfd, const char *path, int flags)
{
    struct stat k; // through syscall(): its wrapper gives the path the rootfs prefix, as fakechroot would
    if (syscall(SYS_newfstatat, dirfd, path, &k, flags) != 0)
        return 0;
    return k.st_mode & S_IFMT;
}
#define FIX_TYPE(st, dirfd, path, flags)                                                                               \
    do {                                                                                                               \
        mode_t t_ = getenv("FAKEROOTKEY") ? real_type((dirfd), (path), (flags)) : 0;                                   \
        if (t_ && ((st)->st_mode & S_IFMT) != t_)                                                                      \
            (st)->st_mode = t_ | ((st)->st_mode & ~S_IFMT);                                                            \
    } while (0)

static int linked_copy(const char *path)
{
    int n = 0;
    struct stat k;
    for (int i = 0; path && i < 16; i++)
        if (linked_copies[i][0][0] && strcmp(linked_copies[i][0], path) == 0 &&
            syscall(SYS_newfstatat, AT_FDCWD, linked_copies[i][1], &k, 0) == 0)
            n++;
    return n;
}

// stat/access on the faked /proc files: programs test them before opening (LibreOffice: "/proc not mounted").
// Answer with the attributes of a readable /proc file.
#include <sys/stat.h>
#define STAT_WRAP(name, type)                                                                                         \
    int name(const char *path, type *st)                                                                               \
    {                                                                                                                  \
        static int (*next)(const char *, type *);                                                                     \
        if (!next)                                                                                                     \
            next = dlsym(RTLD_NEXT, #name);                                                                            \
        char shm_[512];                                                                                                \
        path = dev_shm(path, shm_);                                                                                    \
        int r = next(path, st);                                                                                        \
        if (r == 0)                                                                                                    \
            FIX_TYPE(st, AT_FDCWD, path, strncmp(#name, "lstat", 5) == 0 ? AT_SYMLINK_NOFOLLOW : 0);                   \
        if (r == 0 && st->st_nlink == 1)                                                                               \
            st->st_nlink += linked_copy(path);                                                                         \
        return r < 0 && (errno == EACCES || errno == ENOENT) && IS_UPTIME(path) ? next("/proc/self/stat", st) : r;                          \
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
        char shm_[512];                                                                                                \
        path = dev_shm(path, shm_);                                                                                    \
        int r = next(dirfd, path, st, flags);                                                                          \
        if (r == 0)                                                                                                    \
            FIX_TYPE(st, dirfd, path, flags);                                                                          \
        if (r == 0 && st->st_nlink == 1)                                                                               \
            st->st_nlink += linked_copy(path);                                                                         \
        return r < 0 && (errno == EACCES || errno == ENOENT) && IS_UPTIME(path) ? next(AT_FDCWD, "/proc/self/stat", st, flags) : r;         \
    }
FSTATAT_WRAP(fstatat, struct stat)
FSTATAT_WRAP(fstatat64, struct stat64)

int access(const char *path, int mode)
{
    static int (*next)(const char *, int);
    if (!next)
        next = dlsym(RTLD_NEXT, "access");
    char shm_[512];
    path = dev_shm(path, shm_);
    int r = next(path, mode);
    return r < 0 && (errno == EACCES || errno == ENOENT) && IS_UPTIME(path) && !(mode & (W_OK | X_OK)) ? 0 : r;
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
    int r;
    if (tmpfile_link(olddirfd, old, newdirfd, new, flags, &r))
        return r;
    r = next(olddirfd, old, newdirfd, new, flags);
    if (r < 0 && (errno == EACCES || errno == EPERM)) {
        r = copy_excl(olddirfd, old, newdirfd, new);
        static int slot;
        if (r == 0 && olddirfd == AT_FDCWD && newdirfd == AT_FDCWD && strlen(old) < sizeof linked_copies[0][0] &&
            strlen(new) < sizeof linked_copies[0][1]) {
            strcpy(linked_copies[slot % 16][0], old);
            strcpy(linked_copies[slot++ % 16][1], new);
        }
    }
    return r;
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
    case SYS_shmget: case SYS_shmat: case SYS_shmdt: case SYS_shmctl: case SYS_semget: case SYS_semop:
    case SYS_semtimedop: case SYS_semctl: case SYS_msgget: case SYS_msgsnd: case SYS_msgrcv: case SYS_msgctl:
    case SYS_io_uring_setup: case SYS_io_uring_enter: case SYS_io_uring_register: // libuv (Node, Electron)
    case SYS_name_to_handle_at: case SYS_open_by_handle_at: // systemd
    case 452: // fchmodat2 (systemd calls it raw; glibc's own is patched)
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
    long r = next(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
    // Raw readlinkat("/proc/self/exe") (Electron's utility processes): the loader otherwise, so VS Code's helpers
    // looked for icudtl.dat next to ld.so and died. Same answer as the readlink() wrapper.
    if (nr == SYS_readlinkat && IS_PROC((const char *)a[1]))
        return fix_proc_link((const char *)a[1], (char *)a[2], (size_t)a[3], r);
    return r;
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

// The runtime lives in the environment (LD_PRELOAD, FAKECHROOT_*...). Programs that start children with a cleaned
// environment (VS Code's helper processes) dropped it: no fakechroot, no android-compat in the child, which then saw
// Android's paths (VS Code: "Invalid file descriptor to ICU data", helpers crashed). Put the missing ones back.
static const char *const runtime_vars[] = {"LD_PRELOAD",       "LD_LIBRARY_PATH", "FAKECHROOT_BASE",
                                           "FAKECHROOT_ELFLOADER", "FAKECHROOT_EXCLUDE_PATH", "FAKECHROOT",
                                           "FAKEROOTKEY",      "LOCPATH",         "GCONV_PATH", "TZDIR"};
#define N_RUNTIME (sizeof runtime_vars / sizeof *runtime_vars)
static char *runtime_env[N_RUNTIME]; // "NAME=value" snapshots from our own start

__attribute__((constructor)) static void snapshot_runtime_env(void)
{
    for (size_t i = 0; i < N_RUNTIME; i++) {
        const char *v = getenv(runtime_vars[i]);
        if (v && asprintf(&runtime_env[i], "%s=%s", runtime_vars[i], v) < 0)
            runtime_env[i] = NULL;
    }
}

static int has_var(char *const envp[], const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; envp && envp[i]; i++)
        if (strncmp(envp[i], name, n) == 0 && envp[i][n] == '=')
            return 1;
    return 0;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    static int (*next)(const char *, char *const[], char *const[]);
    if (!next)
        next = dlsym(RTLD_NEXT, "execve");
    size_t n = 0;
    while (envp && envp[n])
        n++;
    char *env[n + N_RUNTIME + 1];
    for (size_t i = 0; i < n; i++)
        env[i] = envp[i];
    for (size_t i = 0; i < N_RUNTIME; i++) {
        if (runtime_env[i] && !has_var(envp, runtime_vars[i]))
            env[n++] = runtime_env[i];
        // fakechroot's execve reads them from our own environment (env -i cleared it): restore, we exec anyway.
        if (runtime_env[i] && !getenv(runtime_vars[i]))
            putenv(runtime_env[i]);
    }
    env[n] = NULL;
    return next(self_exe_target(path), argv, env);
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
// glibc's execvp reaches its internal execve directly: route it through ours (env -i, cleaned environments).
int execvp(const char *file, char *const argv[])
{
    return execvpe(file, argv, environ);
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

// SysV IPC (shmget, semget, msgget...): Android's seccomp kills the process (Dolphin through KDE/Qt's SysV shared
// memory: opened and closed at once). Answer ENOSYS like a kernel without SysV IPC; callers fall back (POSIX shm,
// no MIT-SHM). ponytail: no emulation; add one on /tmp/.shm files if a program has no fallback.
#define NO_IPC(ret, name, ...)                                                                                         \
    ret name(__VA_ARGS__)                                                                                              \
    {                                                                                                                  \
        errno = ENOSYS;                                                                                                \
        return (ret)-1;                                                                                                \
    }
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
NO_IPC(int, shmget, key_t k, size_t s, int f)
NO_IPC(void *, shmat, int id, const void *a, int f)
NO_IPC(int, shmdt, const void *a)
NO_IPC(int, shmctl, int id, int c, struct shmid_ds *b)
NO_IPC(int, semget, key_t k, int n, int f)
NO_IPC(int, semop, int id, struct sembuf *o, size_t n)
NO_IPC(int, semtimedop, int id, struct sembuf *o, size_t n, const struct timespec *t)
NO_IPC(int, semctl, int id, int n, int c, ...)
NO_IPC(int, msgget, key_t k, int f)
NO_IPC(int, msgsnd, int id, const void *m, size_t s, int f)
NO_IPC(ssize_t, msgrcv, int id, void *m, size_t s, long t, int f)
NO_IPC(int, msgctl, int id, int c, struct msqid_ds *b)

// symlink/symlinkat: fakechroot stores an absolute target as a HOST path (the kernel follows links on Android's
// filesystem), so links broke as soon as the rootfs was reached under another name (/data/data vs /data/user/0:
// LibreOffice's launcher, .desktop files...). Write rootfs targets relative to the link instead: valid for the
// kernel and under any rootfs path. Excluded host paths (/dev, /proc, the run dir) stay absolute.
#include <libgen.h>
static int guest_dir_of(int dirfd, const char *linkpath, char *out, size_t len)
{
    char tmp[4096], lp[4096];
    snprintf(lp, sizeof lp, "%s", linkpath);
    const char *d = dirname(lp); // "." for a bare name
    if (d[0] == '/')
        snprintf(tmp, sizeof tmp, "%s", d);
    else {
        char base[4096];
        if (dirfd == AT_FDCWD) {
            if (!getcwd(base, sizeof base)) // fakechroot: guest view
                return -1;
        } else {
            char fdl[64];
            snprintf(fdl, sizeof fdl, "/proc/self/fd/%d", dirfd);
            ssize_t n = readlink(fdl, base, sizeof base - 1); // our wrapper: base stripped
            if (n <= 0)
                return -1;
            base[n] = 0;
        }
        snprintf(tmp, sizeof tmp, "%s/%s", base, d);
    }
    snprintf(out, len, "%s", strip_base(tmp));
    return 0;
}

static const char *relative_target(const char *target, int dirfd, const char *linkpath, char *out)
{
    char t[4096], dir[4096], probe[4096];
    if (!target || target[0] != '/' || !rootfs_base)
        return target;
    snprintf(t, sizeof t, "%s", target);
    strip_base(t);
    if (gs_translate(t, probe) == t) // excluded (/dev, /proc, run dir): stays a host path
        return target;
    if (guest_dir_of(dirfd, linkpath, dir, sizeof dir) < 0)
        return target;
    // Unpacking another system into a subdirectory (build-rootfs-native.sh): its absolute links mean ITS root.
    const char *sub = getenv("ANDROID_COMPAT_SYMLINK_ROOT");
    size_t sl = sub ? strlen(sub) : 0;
    if (sl && strncmp(dir, sub, sl) == 0 && (dir[sl] == '/' || !dir[sl]) && sl + strlen(t) < sizeof t) {
        memmove(t + sl, t, strlen(t) + 1);
        memcpy(t, sub, sl);
    }
    // Common leading components of dir and t, then one ".." per remaining dir component.
    const char *a = dir, *b = t;
    const char *la = a, *lb = b;
    while (*a && *a == *b) {
        if (*a == '/') {
            la = a;
            lb = b;
        }
        a++, b++;
    }
    if (!*a && (*b == '/' || !*b)) // dir is a prefix of t
        la = a, lb = b;
    out[0] = 0;
    for (const char *p = la; *p; p++)
        if (*p == '/' && p[1] && strcmp(p, "/.") != 0)
            strcat(out, "../");
    strcat(out, *lb == '/' ? lb + 1 : lb);
    if (!out[0])
        strcpy(out, ".");
    return out;
}

// realpath on an excluded host path (the session's run dir): fakechroot resolves it component by component inside the
// rootfs, which has no /data, and fails. Quickshell canonicalizes its run dir: empty paths, no instance symlinks, so
// `quickshell kill` found nothing and omarchy-restart-shell stacked a second bar.
// ponytail: returns the path as given (no symlink or ".." resolution); the run dir has neither in practice.
static char *excluded_realpath(const char *path, char *resolved)
{
    char t[4096], probe[4096];
    struct stat st;
    if (!path || path[0] != '/' || !rootfs_base || strlen(path) >= sizeof t)
        return (char *)-1;
    snprintf(t, sizeof t, "%s", path);
    if (gs_translate(t, probe) != t) // a rootfs path: fakechroot's
        return (char *)-1;
    if (stat(path, &st) < 0)
        return NULL;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    if (!resolved && !(resolved = malloc(PATH_MAX)))
        return NULL;
    snprintf(resolved, PATH_MAX, "%.*s", (int)n, path);
    return resolved;
}

char *realpath(const char *path, char *resolved)
{
    static char *(*next)(const char *, char *);
    char *r = excluded_realpath(path, resolved);
    if (r != (char *)-1)
        return r;
    if (!next)
        next = dlsym(RTLD_NEXT, "realpath");
    return next(path, resolved);
}

char *__realpath_chk(const char *path, char *resolved, size_t len)
{
    (void)len;
    return realpath(path, resolved);
}

int symlinkat(const char *target, int dirfd, const char *linkpath)
{
    static int (*next)(const char *, int, const char *);
    char rel[8192];
    if (!next)
        next = dlsym(RTLD_NEXT, "symlinkat");
    if (linkpath && linkpath[0] == '/') // the kernel ignores dirfd then; fakechroot may not (quickshell passes 0)
        dirfd = AT_FDCWD;
    return next(relative_target(target, dirfd, linkpath, rel), dirfd, linkpath);
}

int symlink(const char *target, const char *linkpath)
{
    static int (*next)(const char *, const char *);
    char rel[8192];
    if (!next)
        next = dlsym(RTLD_NEXT, "symlink");
    return next(relative_target(target, AT_FDCWD, linkpath, rel), linkpath);
}

// name_to_handle_at / open_by_handle_at: Android's seccomp kills the process (systemd-detect-virt, systemd-sysusers
// in pacman hooks: "Bad system call"). EOPNOTSUPP is what a filesystem without handles answers; systemd falls back.
struct file_handle;
int name_to_handle_at(int dirfd, const char *path, struct file_handle *h, int *mount_id, int flags)
{
    (void)dirfd, (void)path, (void)h, (void)mount_id, (void)flags;
    errno = EOPNOTSUPP;
    return -1;
}

int open_by_handle_at(int mount_fd, struct file_handle *h, int flags)
{
    (void)mount_fd, (void)h, (void)flags;
    errno = EOPNOTSUPP;
    return -1;
}

// Audit netlink socket: SELinux refuses it to apps (EACCES), and shadow's useradd/usermod/groupadd abort on that
// ("Cannot open audit interface"). Answer like a kernel built without audit, which they accept.
#include <linux/netlink.h>
#include <sys/socket.h>
int socket(int domain, int type, int protocol)
{
    static int (*next)(int, int, int);
    if (domain == AF_NETLINK && protocol == NETLINK_AUDIT) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    if (!next)
        next = dlsym(RTLD_NEXT, "socket");
    return next(domain, type, protocol);
}

// DRM_IOCTL_VERSION on Qualcomm's KGSL GPU device: Android gives apps no DRM render node, but Mesa allocates GBM
// buffers (dmabufs) on /dev/kgsl-3d0. Chromium/Electron (--render-node-override=/dev/kgsl-3d0) first ask the node's
// DRM version and abort without one; answer as the msm driver (freedreno's) so GPU compositing can start.
struct lotfix_drm_version {
    int    version_major, version_minor, version_patchlevel;
    size_t name_len;
    char  *name;
    size_t date_len;
    char  *date;
    size_t desc_len;
    char  *desc;
};
#define LOTFIX_DRM_IOCTL_VERSION _IOWR('d', 0x00, struct lotfix_drm_version)

static int is_kgsl_fd(int fd)
{
    char p[64], t[256];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = syscall(SYS_readlinkat, AT_FDCWD, p, t, sizeof t - 1); // raw: no path rewriting needed
    if (n <= 0)
        return 0;
    t[n] = 0;
    return strstr(t, "/dev/kgsl") != NULL;
}

static void drm_str(char *dst, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (dst && *len)
        memcpy(dst, s, *len < n ? *len : n);
    *len = n;
}

int ioctl(int fd, unsigned long req, ...)
{
    static int (*next)(int, unsigned long, ...);
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!next)
        next = dlsym(RTLD_NEXT, "ioctl");
    // Copy-on-write clones (FICLONE, FICLONERANGE, FIDEDUPERANGE): Android's ioctl allowlist answers EACCES, which
    // callers read as a temporary failure and retry on every file (libostree re-checks and re-opens its repo config for
    // each object it writes: hundreds of times per install). A filesystem without the feature answers EOPNOTSUPP, and
    // callers then ask once and fall back to copying (cp --reflink=auto does too).
    if (req == 0x40049409UL /* FICLONE */ || req == 0x4020940dUL /* FICLONERANGE */ ||
        req == 0xc0189436UL /* FIDEDUPERANGE */) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (req == LOTFIX_DRM_IOCTL_VERSION && arg && is_kgsl_fd(fd)) {
        struct lotfix_drm_version *v = arg;
        v->version_major = 1, v->version_minor = 12, v->version_patchlevel = 0;
        drm_str(v->name, &v->name_len, "msm");
        drm_str(v->date, &v->date_len, "20130625");
        drm_str(v->desc, &v->desc_len, "MSM Snapdragon DRM (KGSL)");
        return 0;
    }
    return next(fd, req, arg);
}

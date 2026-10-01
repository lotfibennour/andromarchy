// Preloaded in every no-proot process. Two /proc views fakechroot gets wrong:
//  1. /proc/self/exe: programs are started as "ld-linux-aarch64.so.1 /usr/bin/prog", so the kernel's link points
//     at the loader. Programs that locate their files next to their binary (Firefox: "Couldn't load XPCOM",
//     Quickshell's relaunch...) look in the glibc dir. Answer with the real program, from argv[0].
//  2. every /proc/*/{fd/N,cwd,exe} link: the kernel answers HOST paths (/data/data/.../rootfs/usr/...). A real
//     chroot shows /usr/...; with the prefix, fakechroot prepends it again when the path is reused.
// Build (rootfs): gcc -O2 -shared -fPIC -o android-compat.so android-compat.c go-syscalls.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *program_invocation_name; // argv[0]

static const char *strip_base(char *path)
{
    const char *base = getenv("FAKECHROOT_BASE");
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

__attribute__((constructor)) static void android_compat_init(void)
{
    if (program_invocation_name && program_invocation_name[0] == '/')
        go_syscalls_init(program_invocation_name, main_program());
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
        return 1;
    default:
        return 0;
    }
}

long syscall(long nr, ...)
{
    static long (*next)(long, ...);
    va_list ap;
    long a[6];
    va_start(ap, nr);
    for (int i = 0; i < 6; i++)
        a[i] = va_arg(ap, long);
    va_end(ap);
    if (android_blocked(nr)) {
        errno = ENOSYS;
        return -1;
    }
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

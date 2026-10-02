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

__attribute__((constructor)) static void android_compat_init(void)
{
    rootfs_base = getenv("FAKECHROOT_BASE");
    if (program_invocation_name && program_invocation_name[0] == '/') {
        go_syscalls_init(program_invocation_name, main_program()); // both resolve the host path first
        // fakechroot's loader exec passes the HOST path as argv[0]. Programs that locate their files from it
        // (gcc: cc1, specs...) then build host paths that fakechroot prefixes a second time. Show the guest path.
        strip_base(program_invocation_name); // in place: argv[0] is the same string
    }
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

// Firefox's parent process probes memfd by reopening it through /proc/self/fd (denied by SELinux) and falls back
// to shm_open, while its content processes probe without reopening, accept memfd, then reject the parent's shm_open
// handles as "not safe to map" and crash on every page. Failing the probe everywhere keeps them consistent.
// ponytail: Firefox-only by name; make /proc/self/fd reopen work instead if other apps hit the same split.
int memfd_create(const char *name, unsigned int flags)
{
    static int (*next)(const char *, unsigned int);
    if (name && strncmp(name, "mozilla-ipc", 11) == 0) {
        errno = ENOSYS;
        return -1;
    }
    if (!next)
        next = dlsym(RTLD_NEXT, "memfd_create");
    return next(name, flags);
}

// 3a. /proc/uptime and /proc/stat: SELinux denies them to apps; uptime, htop and systemctl (process ages from the
//     boot time) need them. Serve them from CLOCK_BOOTTIME in a memfd: uptime with idle 0, stat with a zero "cpu" line
//     and the real "btime". Every other path passes through.
// ponytail: no per-CPU times in the fake /proc/stat; CPU meters (htop, top) stay at zero.
#include <stdarg.h>
#include <sys/mman.h>
#include <time.h>

static int fake_proc_fd(const char *path)
{
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    char buf[128];
    int n = strcmp(path, "/proc/uptime") == 0
                ? snprintf(buf, sizeof buf, "%ld.%02ld 0.00\n", (long)t.tv_sec, t.tv_nsec / 10000000)
                : snprintf(buf, sizeof buf, "cpu  0 0 0 0 0 0 0 0 0 0\nbtime %ld\n", (long)(time(NULL) - t.tv_sec));
    int fd = memfd_create("proc", MFD_CLOEXEC);
    if (fd < 0 || write(fd, buf, n) != n || lseek(fd, 0, SEEK_SET) < 0) {
        if (fd >= 0)
            close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

#define IS_UPTIME(p) ((p) && (strcmp((p), "/proc/uptime") == 0 || strcmp((p), "/proc/stat") == 0))

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
        return next(path, flags, mode);                                                                                 \
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
        return next(dirfd, path, flags, mode);                                                                          \
    }
OPENAT_WRAP(openat)
OPENAT_WRAP(openat64)

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
        return next(path, mode);                                                                                        \
    }
FOPEN_WRAP(fopen)
FOPEN_WRAP(fopen64)

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

// 8. execvpe: fakechroot 2.20.1 does not wrap it, so glibc's version issues the execve syscall itself with the guest
//    path, on Android's filesystem (foot: "bash: failed to execute", or Android's /bin/sh). Search PATH here and go
//    through execve, which fakechroot wraps (loader + #! handling).
extern char **environ;

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

// Preloaded in every no-proot process. Two /proc views fakechroot gets wrong:
//  1. /proc/self/exe: programs are started as "ld-linux-aarch64.so.1 /usr/bin/prog", so the kernel's link points
//     at the loader. Programs that locate their files next to their binary (Firefox: "Couldn't load XPCOM",
//     Quickshell's relaunch...) look in the glibc dir. Answer with the real program, from argv[0].
//  2. every /proc/*/{fd/N,cwd,exe} link: the kernel answers HOST paths (/data/data/.../rootfs/usr/...). A real
//     chroot shows /usr/...; with the prefix, fakechroot prepends it again when the path is reused.
// Build (rootfs): gcc -O2 -shared -fPIC -o android-compat.so android-compat.c -ldl
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

// Path translation for Go programs without proot.
//
// Go issues its syscalls itself (no glibc), so fakechroot's LD_PRELOAD wrappers never see them: a Go program sees
// Android's filesystem, and exec's Android's /bin/sh. For a Go main executable only, this installs a seccomp filter
// that TRAPS (SIGSYS) the path syscalls issued FROM THE GO BINARY'S CODE (instruction pointer in its text segment).
// Syscalls from glibc / ld.so / fakechroot are untouched, so non-Go code keeps full speed, and exec'd children
// (whose own code lives elsewhere) are not affected. The handler rewrites paths with fakechroot's rules, performs
// the syscall itself, and writes the result in x0.
//
// Linked into android-compat.so (call go_syscalls_init() from its constructor). Async-signal-safe: raw syscalls and
// stack buffers only in the handler.
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define PATH_MAX_ 4096

// ---- raw syscalls (from this .so: outside the trapped range) ----
static long raw6(long nr, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
#define raw(nr, ...) raw6_args(nr, __VA_ARGS__, 0, 0, 0, 0, 0, 0)
#define raw6_args(nr, a, b, c, d, e, f, ...) raw6((nr), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

// ---- configuration captured at init (before Go starts) ----
static char base[PATH_MAX_];
static size_t base_len;
static char excludes[16][256];
static int n_excludes;
static char loader[PATH_MAX_];
static char self_exe_guest[PATH_MAX_];
static uintptr_t text_lo, text_hi;
static int trace_on; // GO_SYSCALLS_TRACE=1: log every trapped syscall (debug only, not async-signal-safe)
static struct kernel_sigaction {
    void *handler;
    unsigned long flags;
    void *restorer;
    uint64_t mask;
} go_sigsys_action;                     // what Go asked for SIGSYS via raw rt_sigaction (kept, not installed)
static struct sigaction go_sigsys_user; // same, via libc sigaction() (cgo)

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int starts_with_dir(const char *p, const char *dir, size_t n)
{
    return strncmp(p, dir, n) == 0 && (p[n] == '/' || p[n] == 0);
}

// fakechroot's rule: absolute guest paths get the rootfs prefix, unless already prefixed or excluded.
static const char *translate(const char *p, char *out)
{
    if (!p || p[0] != '/' || starts_with_dir(p, base, base_len))
        return p;
    for (int i = 0; i < n_excludes; i++)
        if (starts_with_dir(p, excludes[i], slen(excludes[i])))
            return p;
    size_t n = slen(p);
    if (base_len + n + 1 > PATH_MAX_)
        return p;
    memcpy(out, base, base_len);
    memcpy(out + base_len, p, n + 1);
    return out;
}

const char *gs_translate(const char *p, char *out)
{
    return translate(p, out);
}

// Inverse for paths the kernel returns (getcwd, readlinkat): show them from inside the rootfs.
static long strip_base(char *buf, long n)
{
    if (n > (long)base_len && strncmp(buf, base, base_len) == 0 && buf[base_len] == '/') {
        memmove(buf, buf + base_len, n - base_len);
        return n - base_len;
    }
    return n;
}

// ---- execve: same as fakechroot's elfloader exec, including #! scripts ----
static long do_execve(const char *path, char *const argv[], char *const envp[])
{
    char hpath[PATH_MAX_], hinterp[PATH_MAX_], head[256];
    const char *host = translate(path, hpath);
    const char *nargv[260];
    int n = 0;

    long fd = raw(SYS_openat, AT_FDCWD, host, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fd;
    long got = raw(SYS_read, fd, head, sizeof head - 1);
    raw(SYS_close, fd);
    if (got < 0)
        return got;
    head[got] = 0;

    nargv[n++] = loader;
    if (got > 2 && head[0] == '#' && head[1] == '!') {
        char *s = head + 2, *e;
        while (*s == ' ' || *s == '\t')
            s++;
        for (e = s; *e && *e != ' ' && *e != '\t' && *e != '\n'; e++)
            ;
        char *arg = NULL;
        if (*e == ' ' || *e == '\t') {
            *e++ = 0;
            while (*e == ' ' || *e == '\t')
                e++;
            arg = e;
            while (*e && *e != '\n')
                e++;
            while (e > arg && (e[-1] == ' ' || e[-1] == '\t'))
                e--;
            *e = 0;
            if (!*arg)
                arg = NULL;
        } else
            *e = 0;
        nargv[n++] = translate(s, hinterp);
        if (arg)
            nargv[n++] = arg;
        nargv[n++] = path; // the interpreter (under fakechroot) opens the script itself
    } else
        nargv[n++] = host;
    for (int i = 1; argv && argv[i] && n < 259; i++)
        nargv[n++] = argv[i];
    nargv[n] = NULL;
    return raw(SYS_execve, loader, nargv, envp);
}

// ---- the trap handler ----
// Every syscall Android's app filter blocks arrives here (SECCOMP_RET_TRAP, si_code SYS_SECCOMP): answer it like a
// kernel would to an unprivileged user without the feature, instead of letting the process die. Measured on the device
// with tools/seccomp-probe.c (126 named syscalls). Privileged ones -> EPERM; everything else -> ENOSYS, which
// libraries treat as an older kernel and fall back from.
static int android_errno(long nr)
{
    switch (nr) {
    case SYS_mount: case SYS_umount2: case SYS_chroot: case SYS_vhangup: case SYS_acct: case SYS_kexec_load:
    case SYS_kexec_file_load: case SYS_init_module: case SYS_finit_module: case SYS_delete_module:
    case SYS_clock_settime: case SYS_settimeofday: case SYS_adjtimex: case SYS_clock_adjtime: case SYS_sethostname:
    case SYS_setdomainname: case SYS_swapon: case SYS_swapoff: case SYS_syslog: case SYS_setuid: case SYS_setgid:
    case SYS_setreuid: case SYS_setregid: case SYS_setresgid: case SYS_setgroups: case SYS_open_tree:
    case SYS_move_mount: case SYS_fsopen: case SYS_fsconfig: case SYS_fsmount: case SYS_fspick: case SYS_mount_setattr:
        return EPERM;
    default:
        return ENOSYS;
    }
}

static void on_sigsys(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uint64_t *r = (uint64_t *)uc->uc_mcontext.regs;
    char p1[PATH_MAX_], p2[PATH_MAX_];
    long ret;

    if (si->si_code != SYS_SECCOMP) { // a genuine SIGSYS: hand it to whoever Go registered (cgo or raw syscall)
        if (go_sigsys_user.sa_sigaction && (void *)go_sigsys_user.sa_handler != SIG_DFL &&
            (void *)go_sigsys_user.sa_handler != SIG_IGN)
            go_sigsys_user.sa_sigaction(sig, si, ctx);
        else if (go_sigsys_action.handler && go_sigsys_action.handler != SIG_DFL && go_sigsys_action.handler != SIG_IGN)
            ((void (*)(int, siginfo_t *, void *))go_sigsys_action.handler)(sig, si, ctx);
        return;
    }

    switch (si->si_syscall) {
    case SYS_openat:
    case SYS_newfstatat:
    case SYS_faccessat:
    case SYS_mkdirat:
    case SYS_mknodat:
    case SYS_unlinkat:
    case SYS_fchmodat:
    case SYS_fchownat:
    case SYS_statx:
        ret = raw6(si->si_syscall, r[0], (long)translate((const char *)r[1], p1), r[2], r[3], r[4], r[5]);
        break;
    case SYS_utimensat:
        ret = raw6(si->si_syscall, r[0], r[1] ? (long)translate((const char *)r[1], p1) : 0, r[2], r[3], 0, 0);
        break;
    case SYS_readlinkat: {
        const char *p = (const char *)r[1];
        if (strcmp(p, "/proc/self/exe") == 0 && self_exe_guest[0]) {
            size_t n = slen(self_exe_guest);
            if (n > r[3])
                n = r[3];
            memcpy((void *)r[2], self_exe_guest, n);
            ret = n;
            break;
        }
        ret = raw(SYS_readlinkat, r[0], translate(p, p1), r[2], r[3]);
        if (ret > 0)
            ret = strip_base((char *)r[2], ret);
        break;
    }
    case SYS_symlinkat: // the link's content stays as written (guest view); only its location is translated
        ret = raw(SYS_symlinkat, r[0], r[1], translate((const char *)r[2], p1));
        break;
    case SYS_linkat:
    case SYS_renameat:
    case SYS_renameat2:
        ret = raw6(si->si_syscall, r[0], (long)translate((const char *)r[1], p1), r[2],
                   (long)translate((const char *)r[3], p2), r[4], 0);
        break;
    case SYS_truncate:
    case SYS_statfs:
    case SYS_chdir:
        ret = raw6(si->si_syscall, (long)translate((const char *)r[0], p1), r[1], r[2], 0, 0, 0);
        break;
    case SYS_inotify_add_watch:
        ret = raw(SYS_inotify_add_watch, r[0], translate((const char *)r[1], p1), r[2]);
        break;
    case SYS_getcwd:
        ret = raw(SYS_getcwd, r[0], r[1]);
        if (ret > 0) { // returns length including the NUL
            long n = strip_base((char *)r[0], ret - 1);
            ((char *)r[0])[n] = 0;
            ret = n + 1;
        }
        break;
    case SYS_execve:
        ret = do_execve((const char *)r[0], (char *const *)r[1], (char *const *)r[2]);
        break;
    case SYS_accept: // killed by Android's filter; same call
        ret = raw(SYS_accept4, r[0], r[1], r[2], 0);
        break;
    case SYS_rt_sigaction: // Go wants SIGSYS for itself: remember it, keep ours installed
        if (r[2])
            memcpy((void *)r[2], &go_sigsys_action, sizeof go_sigsys_action);
        if (r[1])
            memcpy(&go_sigsys_action, (void *)r[1], sizeof go_sigsys_action);
        ret = 0;
        break;
    case SYS_rt_sigprocmask: { // never let Go block SIGSYS (a blocked trap signal kills the process)
        uint64_t set;
        if (r[1]) {
            memcpy(&set, (void *)r[1], sizeof set);
            if (r[0] != SIG_UNBLOCK)
                set &= ~(1ULL << (SIGSYS - 1));
        }
        ret = raw(SYS_rt_sigprocmask, r[0], r[1] ? (long)&set : 0, r[2], r[3]);
        break;
    }
    default: // blocked by Android (our Go filter only traps the cases above): never re-issue it, it would trap again
        ret = -android_errno(si->si_syscall);
    }
    if (trace_on) {
        char msg[600];
        const char *p = (si->si_syscall == SYS_execve || si->si_syscall == SYS_chdir || si->si_syscall == SYS_truncate ||
                         si->si_syscall == SYS_statfs)
                            ? (const char *)r[0]
                        : (si->si_syscall == SYS_getcwd || si->si_syscall == SYS_rt_sigprocmask) ? ""
                                                                                                : (const char *)r[1];
        int m = snprintf(msg, sizeof msg, "go-trap nr=%d path=%.300s -> %ld\n", si->si_syscall, p ? p : "(null)", ret);
        raw(SYS_write, 2, msg, m);
    }
    r[0] = (uint64_t)ret;
}

// cgo blocks ALL signals with pthread_sigmask() before pthread_create, so new Go threads would start with SIGSYS
// blocked, and the first trapped syscall would kill the process. Those calls come from C (not the trapped Go text):
// filter them here once the trap is active.
#include <dlfcn.h>
static int active;

static const sigset_t *without_sigsys(int how, const sigset_t *set, sigset_t *copy)
{
    if (!active || !set || how == SIG_UNBLOCK)
        return set;
    *copy = *set;
    sigdelset(copy, SIGSYS);
    return copy;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    static int (*next)(int, const sigset_t *, sigset_t *);
    sigset_t copy;
    if (!next)
        next = dlsym(RTLD_NEXT, "pthread_sigmask");
    return next(how, without_sigsys(how, set, &copy), old);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    static int (*next)(int, const sigset_t *, sigset_t *);
    sigset_t copy;
    if (!next)
        next = dlsym(RTLD_NEXT, "sigprocmask");
    return next(how, without_sigsys(how, set, &copy), old);
}

// With cgo, Go installs its signal handlers through libc's sigaction() (x_cgo_sigaction), not a raw syscall: keep
// ours for SIGSYS, remember Go's (go_sigsys_user) to forward genuine (non-seccomp) SIGSYS to it.

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    static int (*next)(int, const struct sigaction *, struct sigaction *);
    if (!next)
        next = dlsym(RTLD_NEXT, "sigaction");
    if (!active || sig != SIGSYS)
        return next(sig, act, old);
    if (old)
        *old = go_sigsys_user;
    if (act)
        go_sigsys_user = *act;
    return 0;
}

// ---- setup ----
static int is_go_binary(const char *path)
{
    // raw syscalls: this runs from a constructor, possibly before fakechroot's own wrappers are initialized
    long fd = raw(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    Elf64_Ehdr eh;
    int go = 0;
    if (raw(SYS_pread64, fd, &eh, sizeof eh, 0) == sizeof eh && memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_shnum &&
        eh.e_shnum < 512) {
        Elf64_Shdr sh[512], strsec;
        if (raw(SYS_pread64, fd, sh, eh.e_shnum * sizeof(Elf64_Shdr), eh.e_shoff) == (long)(eh.e_shnum * sizeof(Elf64_Shdr))) {
            strsec = sh[eh.e_shstrndx];
            char names[8192];
            size_t len = strsec.sh_size < sizeof names ? strsec.sh_size : sizeof names;
            if (raw(SYS_pread64, fd, names, len, strsec.sh_offset) == (long)len)
                for (int i = 0; i < eh.e_shnum && !go; i++)
                    // Bun executables (Claude Code, opencode...) also issue inline syscalls from their own text (Zig);
                    // so does Free Pascal's RTL, even linked to libc (Lazarus apps: 2048 could not create its config)
                    if (sh[i].sh_name < len && (strcmp(names + sh[i].sh_name, ".go.buildinfo") == 0 ||
                                                strcmp(names + sh[i].sh_name, ".bun") == 0 ||
                                                strcmp(names + sh[i].sh_name, ".fpcdata") == 0))
                        go = 1;
        }
    }
    raw(SYS_close, fd);
    return go;
}

static int main_text_range(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size, (void)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            text_lo = info->dlpi_addr + ph->p_vaddr;
            text_hi = text_lo + ph->p_memsz;
            return 1;
        }
    }
    return 0;
}

// Calls of interest: path syscalls, Android-blocked ones, and the signal calls Go would use to take SIGSYS away.
static const int trapped[] = {SYS_openat,   SYS_newfstatat,   SYS_faccessat,  SYS_faccessat2, SYS_mkdirat,
                              SYS_mknodat,  SYS_unlinkat,     SYS_fchmodat,   SYS_fchownat,   SYS_statx,
                              SYS_utimensat, SYS_readlinkat,  SYS_symlinkat,  SYS_linkat,     SYS_renameat,
                              SYS_renameat2, SYS_truncate,    SYS_statfs,     SYS_chdir,      SYS_inotify_add_watch,
                              SYS_getcwd,   SYS_execve,       SYS_accept,     SYS_rt_sigprocmask,
                              SYS_set_robust_list, SYS_rseq, SYS_kcmp};

void go_syscalls_init(const char *program, const char *self_guest)
{
    const char *b = getenv("FAKECHROOT_BASE"), *l = getenv("FAKECHROOT_ELFLOADER");
    int dbg = getenv("GO_SYSCALLS_DEBUG") != NULL;
    if (dbg) {
        char msg[512];
        int m = snprintf(msg, sizeof msg, "go-syscalls: init %s base=%d loader=%d go=%d\n", program ? program : "-",
                         b != NULL, l != NULL, program && program[0] == '/' ? is_go_binary(program) : -1);
        raw(SYS_write, 2, msg, m);
    }
    // Base and excludes serve every process (gs_translate, used by android-compat's syscall()); the trap only Go.
    if (b && !base_len) {
        strncpy(base, b, sizeof base - 1);
        base_len = slen(base);
        const char *ex = getenv("FAKECHROOT_EXCLUDE_PATH");
        while (ex && *ex && n_excludes < 16) {
            const char *e = strchr(ex, ':');
            size_t n = e ? (size_t)(e - ex) : slen(ex);
            if (n && n < sizeof excludes[0]) {
                memcpy(excludes[n_excludes], ex, n);
                excludes[n_excludes++][n] = 0;
            }
            ex = e ? e + 1 : NULL;
        }
    }
    // Every process: Android-blocked syscalls become errors instead of SIGSYS deaths (see android_errno). The program
    // may still install its own SIGSYS handler: genuine (non-seccomp) SIGSYS are forwarded to it (sigaction wrapper).
    if (!active) {
        struct sigaction sa = {0};
        sa.sa_sigaction = on_sigsys;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigaction(SIGSYS, &sa, NULL);
        uint64_t sys = 1ULL << (SIGSYS - 1); // a blocked trap signal kills: make sure it is not inherited blocked
        raw(SYS_rt_sigprocmask, SIG_UNBLOCK, &sys, 0, 8);
        active = 1;
    }
    if (!b || !l || !program || program[0] != '/' || !is_go_binary(program))
        return;
    dl_iterate_phdr(main_text_range, NULL);
    if (!text_lo || (text_lo >> 32) != ((text_hi - 1) >> 32))
        return; // text must sit within one 4 GiB window for the 32-bit BPF compare

    strncpy(loader, l, sizeof loader - 1);
    if (self_guest)
        strncpy(self_exe_guest, self_guest, sizeof self_exe_guest - 1);
    // (the SIGSYS handler is already installed above, for every process)

    // BPF: arch check, then for the listed syscalls TRAP iff IP is inside the Go text; rt_sigaction only for SIGSYS.
    struct sock_filter f[128];
    int n = 0;
    uint32_t ip_hi = text_lo >> 32, lo = (uint32_t)text_lo, hi = (uint32_t)text_hi;
#define S(code, k) (f[n++] = (struct sock_filter)BPF_STMT((code), (k)))
#define J(code, k, jt, jf) (f[n++] = (struct sock_filter)BPF_JUMP((code), (k), (jt), (jf)))
    S(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    J(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
    S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    S(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    // rt_sigaction(SIGSYS, ...) -> check IP
    J(BPF_JMP | BPF_JEQ | BPF_K, SYS_rt_sigaction, 0, 3);
    S(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
    J(BPF_JMP | BPF_JEQ | BPF_K, SIGSYS, 2 + (int)(sizeof trapped / sizeof *trapped), 0);
    S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    int nt = sizeof trapped / sizeof *trapped;
    for (int i = 0; i < nt; i++) // match -> jump to the IP check that follows the list
        J(BPF_JMP | BPF_JEQ | BPF_K, trapped[i], (uint8_t)(nt - i), 0);
    S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    // IP check
    S(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer) + 4);
    J(BPF_JMP | BPF_JEQ | BPF_K, ip_hi, 1, 0);
    S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    S(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer));
    J(BPF_JMP | BPF_JGE | BPF_K, lo, 1, 0);
    S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    J(BPF_JMP | BPF_JGE | BPF_K, hi, 0, 1);
    S(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    S(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
#undef S
#undef J
    struct sock_fprog prog = {(unsigned short)n, f};
    long nnp = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    trace_on = getenv("GO_SYSCALLS_TRACE") != NULL;
    long rc = raw(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
    if (getenv("GO_SYSCALLS_DEBUG")) {
        char msg[256];
        int m = snprintf(msg, sizeof msg, "go-syscalls: %s text=%lx-%lx nnp=%ld seccomp=%ld insns=%d\n", program,
                         (unsigned long)text_lo, (unsigned long)text_hi, nnp, rc, n);
        raw(SYS_write, 2, msg, m);
    }
}

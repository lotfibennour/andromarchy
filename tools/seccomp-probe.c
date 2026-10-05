// Which syscalls does Android's app seccomp filter kill? Tries every arm64 syscall number in a throwaway child,
// straight through `svc` (our patched glibc would answer ENOSYS first), and reports the ones that die of SIGSYS.
// Must run inside the app's session (a process under the app's filter), e.g. via `hyprctl dispatch exec`.
// Usage: seccomp-probe [max_nr] > list   (one "nr" per killed syscall; arm64 numbers)
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long raw(long nr)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0, x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0, x4 __asm__("x4") = 0, x5 __asm__("x5") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

int main(int argc, char **argv)
{
    int max = argc > 1 ? atoi(argv[1]) : 470;
    for (int nr = 0; nr <= max; nr++) {
        // Never call these even in a child: they would affect the parent or the session.
        if (nr == 129 /* kill */ || nr == 130 /* tkill */ || nr == 131 /* tgkill */ || nr == 142 /* reboot */ ||
            nr == 164 /* setrlimit */ || nr == 261 /* prlimit64 */)
            continue;
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGSYS, SIG_DFL);
            alarm(1); // calls that block with zero arguments (ppoll...) end by SIGALRM, not SIGSYS
            raw(nr);
            _exit(0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSYS)
            printf("%d\n", nr);
    }
    return 0;
}

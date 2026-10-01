// Debug helper: preloaded into one process, it pauses at startup so a native (Termux) gdb can attach
// before the crash. Build in the rootfs: gcc -shared -fPIC -o wait-for-gdb.so wait-for-gdb.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((constructor)) static void wait_for_gdb(void)
{
    const char *s = getenv("WAIT_FOR_GDB");
    if (!s)
        return;
    fprintf(stderr, "wait-for-gdb: pid %d, sleeping %ss\n", getpid(), s);
    sleep(atoi(s));
}

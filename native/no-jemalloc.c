// Preloaded before a program's own dependencies, this routes the malloc family back to glibc, so a binary
// linked against jemalloc (e.g. quickshell) does not use it. Under fakechroot, jemalloc's heap ends up corrupted
// (crash in je free/malloc, or glibc "buffer overflow detected"); glibc's malloc is fine.
// Build (rootfs): gcc -O2 -shared -fPIC -o no-jemalloc.so no-jemalloc.c
#include <stddef.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void  __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);
extern size_t malloc_usable_size(void *); // glibc's, resolved below via the same name in libc

void *malloc(size_t n) { return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { return __libc_realloc(p, n); }
void  free(void *p) { __libc_free(p); }
void *memalign(size_t a, size_t n) { return __libc_memalign(a, n); }
void *aligned_alloc(size_t a, size_t n) { return __libc_memalign(a, n); }
int   posix_memalign(void **out, size_t a, size_t n)
{
    void *p = __libc_memalign(a, n);
    if (!p)
        return 12; // ENOMEM
    *out = p;
    return 0;
}

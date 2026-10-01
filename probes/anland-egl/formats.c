// Lists the dmabuf formats/modifiers the KGSL Mesa reports on the surfaceless platform (what Hyprland queries).
// Build: gcc formats.c -lEGL
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>

int main(void)
{
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(dpy, NULL, NULL)) {
        printf("eglInitialize failed 0x%x\n", eglGetError());
        return 1;
    }
    PFNEGLQUERYDMABUFFORMATSEXTPROC qf = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC qm = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    EGLint n = 0;
    if (!qf || !qf(dpy, 0, NULL, &n)) {
        printf("eglQueryDmaBufFormatsEXT failed 0x%x\n", eglGetError());
        return 1;
    }
    printf("%d formats\n", n);
    EGLint fmts[256];
    qf(dpy, n < 256 ? n : 256, fmts, &n);
    for (int i = 0; i < n; i++) {
        EGLint m = 0;
        EGLBoolean ok = qm(dpy, fmts[i], 0, NULL, NULL, &m);
        printf("  %.4s mods=%d%s\n", (char *)&fmts[i], m, ok ? "" : " (query failed)");
    }
    return 0;
}

// Phase A.1 probe: can a glibc (Arch) process import Anland's dmabufs into EGL and draw?
// Cycles red/green/blue full-screen for ~5 s. Build: gcc proto.c display_producer.c socket_utils.c -lEGL -lGLESv2
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "display_producer.h"

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

int main(void)
{
    const char *sock = getenv("ANLAND_SOCKET");
    if (!sock) DIE("ANLAND_SOCKET not set");

    display_ctx *ctx;
    if (connect_to_deamon(&ctx, sock) < 0) DIE("connect_to_deamon failed");
    uint32_t w, h, fmt, refresh;
    get_screen_info(ctx, &w, &h, &fmt, &refresh);
    printf("screen %ux%u fmt=%u refresh=%u mHz\n", w, h, fmt, refresh);

    // ponytail: busy-wait, the real backend uses the fallback callback + a timer.
    for (int i = 0; try_exit_fallback(ctx) != 0; i++) {
        if (i == 600) DIE("consumer never connected: is the Anland Termux app in front?");
        usleep(100000);
    }
    int n = get_buf_count(ctx);
    printf("consumer connected, %d buffers\n", n);

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(dpy, NULL, NULL)) DIE("eglInitialize failed: 0x%x", eglGetError());
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext egl = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
    if (egl == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl))
        DIE("context failed: 0x%x", eglGetError());
    printf("GL: %s\n", glGetString(GL_RENDERER));

    GLuint fbo[8];
    for (int i = 0; i < n && i < 8; i++) {
        struct buf_info info;
        int fd = get_dmabuf_fd_at(ctx, i);
        if (fd < 0 || get_dmabuf_info_at(ctx, i, &info) < 0) DIE("buffer %d: no info", i);
        uint32_t drm = info.format == 1 ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XRGB8888;
        EGLAttrib a[] = {
            EGL_WIDTH, info.width, EGL_HEIGHT, info.height,
            EGL_LINUX_DRM_FOURCC_EXT, drm,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, info.offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, info.stride,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLAttrib)(info.modifier & 0xffffffff),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLAttrib)(info.modifier >> 32),
            EGL_NONE};
        if (info.modifier == DRM_FORMAT_MOD_INVALID) a[12] = EGL_NONE; // no modifier attrs
        printf("buffer %d: %ux%u stride=%u fmt=%u fourcc=0x%x mod=0x%llx off=%u fd=%d\n", i, info.width, info.height,
               info.stride, info.format, drm, (unsigned long long)info.modifier, info.offset, fd);
        EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
        if (img == EGL_NO_IMAGE && getenv("PROTO_NO_MOD")) { a[12] = EGL_NONE; img = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a); }
        if (img == EGL_NO_IMAGE) DIE("buffer %d: eglCreateImage failed 0x%x", i, eglGetError());
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        ((PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES"))(GL_TEXTURE_2D, img);
        glGenFramebuffers(1, &fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) DIE("buffer %d: FBO incomplete", i);
        printf("buffer %d imported: %ux%u stride=%u fourcc=0x%x mod=0x%llx\n",
               i, info.width, info.height, info.stride, drm, (unsigned long long)info.modifier);
    }

    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    const float colors[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    int ready = get_buffer_ready_fd(ctx), frames = 0;
    for (int f = 0; f < 300; f++) {
        int idx = get_selected_idx(ctx);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[idx]);
        glViewport(0, 0, w, h);
        const float *c = colors[(f / 100) % 3];
        glClearColor(c[0], c[1], c[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        EGLSync s = eglCreateSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        set_render_fence(ctx, dupFence(dpy, s));
        eglDestroySync(dpy, s);
        trigger_refresh(ctx);
        struct pollfd p = {ready, POLLIN, 0};
        if (poll(&p, 1, 1000) > 0) { eventfd_t v; eventfd_read(ready, &v); frames++; }
        if (is_fallback(ctx)) DIE("consumer lost after %d frames", frames);
    }
    printf("OK: %d frames acknowledged by the consumer\n", frames);
    disconnect(ctx);
    return 0;
}

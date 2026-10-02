#define _GNU_SOURCE
#include "camera_service.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/sharedmem.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImageReader.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#define TAG "AnlandCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Fallback slot size when a camera's max resolution is unknown (1080p I420). */
#define CAM_FALLBACK_BYTES (1920 * 1080 * 3 / 2)

/*
 * Single global instance. Created once in camera_service_init() and persistent for
 * the process. Per camera we own: a SEQPACKET stream socketpair (small control/pacing
 * messages, the producer gets the remote end via SCM_RIGHTS) and an ashmem region of
 * CAMERA_SLOTS * slot_bytes holding the NV21 double buffer. The camera is driven
 * entirely in C via the Camera2 NDK: an AImageReader delivers YUV_420_888 frames on
 * the NDK's reader thread, on_frame_available() packs them straight into a slot, and
 * the io thread does the READY/DONE handshake with the producer.
 */
static struct camera_state {
    int ctrl_local;
    int ctrl_remote;
    int stream_local[MAX_CAMERAS];
    int stream_remote[MAX_CAMERAS];
    int num_cameras;

    /* { ctrl_remote, stream_remote[0..N-1] } -- handed to the producer verbatim. */
    int alloc_fds[1 + MAX_CAMERAS];

    /* Per-camera shared-memory double buffer. */
    int      shm_fd[MAX_CAMERAS];
    uint8_t *shm_ptr[MAX_CAMERAS];           /* mmap, CAMERA_SLOTS * slot_bytes */
    size_t   slot_bytes[MAX_CAMERAS];

    /* Slot ownership handshake: slot_free[c][s] is true once the producer has DONE'd
     * that slot (or it has never been handed out), meaning we may write it. */
    pthread_mutex_t slot_lock[MAX_CAMERAS];
    pthread_cond_t  slot_cond[MAX_CAMERAS];
    bool            slot_free[MAX_CAMERAS][CAMERA_SLOTS];
    int             cur_slot[MAX_CAMERAS];   /* next slot on_frame_available fills */

    /* --- Camera2 NDK state --- */
    ACameraManager *cam_mgr;
    char           *cam_ids[MAX_CAMERAS];    /* strdup'd camera id strings */
    int             max_w[MAX_CAMERAS];      /* max YUV_420_888 output size  */
    int             max_h[MAX_CAMERAS];

    /* Per-camera capture pipeline (NULL when not recording). */
    pthread_mutex_t cap_lock[MAX_CAMERAS];   /* guard start/stop races */
    ACameraDevice               *device[MAX_CAMERAS];
    ACameraCaptureSession       *session[MAX_CAMERAS];
    AImageReader                *reader[MAX_CAMERAS];
    ANativeWindow               *window[MAX_CAMERAS];     /* owned by reader */
    ACaptureSessionOutputContainer *out_container[MAX_CAMERAS];
    ACaptureSessionOutput       *session_output[MAX_CAMERAS];
    ACameraOutputTarget         *out_target[MAX_CAMERAS];
    ACaptureRequest             *request[MAX_CAMERAS];
    /* Callback structs must outlive the calls that register them. */
    AImageReader_ImageListener      img_listener[MAX_CAMERAS];
    ACameraDevice_StateCallbacks    dev_cb[MAX_CAMERAS];
    ACameraCaptureSession_stateCallbacks ses_cb[MAX_CAMERAS];

    pthread_t      io_thread;
    volatile bool  running;
    /* Read by do_connect() on the render thread; written on the main thread in
     * init/destroy. volatile so the render thread sees a fresh value when it
     * checks camera_service_is_ready() right after nativeStart. */
    volatile bool  ready;
} g_camera;

/* forward decls */
static void cam_stop_locked(int id);

/* Read exactly len bytes (blocking) from fd. Returns 0 on success, -1 on error. */
static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

/* Send a fixed stream-control message, optionally with an fd attached (SCM_RIGHTS). */
static void send_stream(int sock, uint8_t type, uint8_t slot, uint16_t fmt,
                        uint32_t a, uint32_t b, int fd)
{
    struct cam_stream_msg m = { .type = type, .slot = slot, .fmt = fmt,
                                .a = a, .b = b };
    struct iovec iov = { .iov_base = &m, .iov_len = sizeof(m) };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        msg.msg_control = cmsg.buf;
        msg.msg_controllen = sizeof(cmsg.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(sock, &msg, MSG_NOSIGNAL) < 0)
        LOGE("stream: send type=%u failed: %s", type, strerror(errno));
}

/* ---------------------------------------------------------------
 * Slot handshake + NV21 packing (was the nativeAwaitSlotFree /
 * nativePackFrame / nativeFrameReady JNI trio; now plain C, driven
 * by on_frame_available on the NDK reader thread).
 * --------------------------------------------------------------- */

/* Block until the producer has released `slot` (DONE) or timeoutMs elapses, then claim
 * it for writing. Returns 0 if the slot was free, -1 on timeout (claimed anyway, so we
 * drop/overwrite rather than stall forever when nothing is consuming). */
static int cam_await_slot_free(int cam, int slot, int timeoutMs)
{
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return -1;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    pthread_mutex_lock(&g_camera.slot_lock[cam]);
    while (!g_camera.slot_free[cam][slot]) {
        if (pthread_cond_timedwait(&g_camera.slot_cond[cam],
                                   &g_camera.slot_lock[cam], &ts) == ETIMEDOUT) {
            rc = -1;
            break;
        }
    }
    g_camera.slot_free[cam][slot] = false;   /* claim it for writing */
    pthread_mutex_unlock(&g_camera.slot_lock[cam]);
    return rc;
}

/* Notify the producer that `slot` holds a fresh frame it can copy out. */
static void cam_frame_ready(int cam, int slot, int w, int h, int fmt)
{
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return;
    send_stream(g_camera.stream_local[cam], CAM_STREAM_READY, (uint8_t)slot,
                (uint16_t)fmt, (uint32_t)w, (uint32_t)h, -1);
}

/*
 * Pack a YUV_420_888 frame from the AImage plane buffers into the shm slot as NV21
 * (Y plane + interleaved V,U), which is the producer's fixed node format. NV21 is the
 * overwhelming Android default, so the common path is a verbatim chroma memcpy with no
 * de-interleave; the rare NV12 / planar sources are converted with a light per-pair
 * loop. Returns CAM_FMT_NV21. Bounds are guaranteed: w*h*3/2 <= slot_bytes (sized for
 * the camera max).
 */
static int cam_pack_frame(int cam, int slot,
                          const uint8_t *y, int yRow,
                          const uint8_t *u, int uRow, int uPix,
                          const uint8_t *v, int vRow, int vPix,
                          int w, int h)
{
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return CAM_FMT_I420;
    if (!y || !u || !v)
        return CAM_FMT_I420;

    uint8_t *dst = g_camera.shm_ptr[cam] + (size_t)slot * g_camera.slot_bytes[cam];

    size_t ySize = (size_t)w * h;
    if (ySize + ySize / 2 > g_camera.slot_bytes[cam])
        return CAM_FMT_I420;   /* shouldn't happen (slot sized for max) */

    uint8_t *p = dst;
    if (yRow == w) {
        memcpy(p, y, ySize);
        p += ySize;
    } else {
        for (int r = 0; r < h; r++) {
            memcpy(p, y + (size_t)r * yRow, (size_t)w);
            p += w;
        }
    }

    int chh = h / 2;
    int cw = w / 2;
    if (uPix == 2 && vPix == 2 && v < u) {
        /* Source is NV21 (V before U): the interleaved V,U block is already NV21. */
        for (int r = 0; r < chh; r++) {
            memcpy(p, v + (size_t)r * vRow, (size_t)w);
            p += w;
        }
    } else if (uPix == 2 && vPix == 2) {
        /* Source is NV12 (U,V interleaved): swap each pair to V,U. */
        for (int r = 0; r < chh; r++) {
            const uint8_t *su = u + (size_t)r * uRow;  /* U at [0], V at [1] */
            for (int k = 0; k < cw; k++) {
                p[2 * k]     = su[2 * k + 1];  /* V */
                p[2 * k + 1] = su[2 * k];      /* U */
            }
            p += w;
        }
    } else {
        /* Planar source (pixelStride 1): interleave V,U. */
        for (int r = 0; r < chh; r++) {
            const uint8_t *su = u + (size_t)r * uRow;
            const uint8_t *sv = v + (size_t)r * vRow;
            for (int k = 0; k < cw; k++) {
                p[2 * k]     = sv[k];  /* V */
                p[2 * k + 1] = su[k];  /* U */
            }
            p += w;
        }
    }
    return CAM_FMT_NV21;
}

/*
 * AImageReader callback. Runs on the NDK's reader thread. Acquires the latest image,
 * packs its planes into the shared-memory slot via the slot handshake, then deletes
 * the image. `context` carries the logical camera index.
 */
static void on_frame_available(void *context, AImageReader *reader)
{
    int cam = (int)(intptr_t)context;
    if (cam < 0 || cam >= g_camera.num_cameras)
        return;

    AImage *image = NULL;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || image == NULL)
        return;

    int32_t numPlanes = 0;
    AImage_getNumberOfPlanes(image, &numPlanes);
    if (numPlanes < 3) {
        LOGE("on_frame_available: cam=%d expected 3 planes, got %d", cam, numPlanes);
        AImage_delete(image);
        return;
    }

    int32_t w = 0, h = 0;
    AImage_getWidth(image, &w);
    AImage_getHeight(image, &h);

    uint8_t *yData = NULL, *uData = NULL, *vData = NULL;
    int yLen = 0, uLen = 0, vLen = 0;
    int32_t yRow = 0, uRow = 0, vRow = 0;
    int32_t yPix = 0, uPix = 0, vPix = 0;
    AImage_getPlaneData(image, 0, &yData, &yLen);
    AImage_getPlaneData(image, 1, &uData, &uLen);
    AImage_getPlaneData(image, 2, &vData, &vLen);
    AImage_getPlaneRowStride(image, 0, &yRow);
    AImage_getPlaneRowStride(image, 1, &uRow);
    AImage_getPlaneRowStride(image, 2, &vRow);
    AImage_getPlanePixelStride(image, 0, &yPix);
    AImage_getPlanePixelStride(image, 1, &uPix);
    AImage_getPlanePixelStride(image, 2, &vPix);
    (void)yPix;

    if (yData && uData && vData) {
        int slot = g_camera.cur_slot[cam];
        cam_await_slot_free(cam, slot, 1000);
        int fmt = cam_pack_frame(cam, slot,
                                 yData, yRow,
                                 uData, uRow, uPix,
                                 vData, vRow, vPix,
                                 w, h);
        cam_frame_ready(cam, slot, w, h, fmt);
        g_camera.cur_slot[cam] = slot ^ 1;
    }

    AImage_delete(image);
}

/* ---------------------------------------------------------------
 * Camera2 NDK device callbacks (no-op / logging; the open and
 * session-create calls themselves are synchronous in the NDK).
 * --------------------------------------------------------------- */

static void on_device_disconnected(void *context, ACameraDevice *dev)
{
    (void)dev;
    LOGE("camera %d disconnected", (int)(intptr_t)context);
}

static void on_device_error(void *context, ACameraDevice *dev, int error)
{
    (void)dev;
    LOGE("camera %d device error=%d", (int)(intptr_t)context, error);
}

static void on_session_closed(void *context, ACameraCaptureSession *s)
{
    (void)context;
    (void)s;
}
static void on_session_ready(void *context, ACameraCaptureSession *s)
{
    (void)context;
    (void)s;
}
static void on_session_active(void *context, ACameraCaptureSession *s)
{
    (void)context;
    (void)s;
}

/* ---------------------------------------------------------------
 * start / stop recording (called from the io thread's ctrl handler;
 * was a JNI CallVoidMethod into Java CameraServices).
 * --------------------------------------------------------------- */

/* Open the camera and start streaming YUV frames into the shm slots. Synchronous:
 * the NDK open / createCaptureSession calls return the live objects directly. Must
 * hold cap_lock[id]. */
static void cam_start_locked(int id, int width, int height)
{
    cam_stop_locked(id);   /* if already running, tear down first */

    int w = (width  > 0) ? width  : g_camera.max_w[id];
    int h = (height > 0) ? height : g_camera.max_h[id];
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    /* 1. AImageReader (YUV_420_888, 2 images for ping-pong). CPU_READ_OFTEN hints
     * gralloc toward a cached, CPU-friendly layout for the per-frame pack copy. */
    media_status_t ms = AImageReader_newWithUsage(
            w, h, AIMAGE_FORMAT_YUV_420_888,
            AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 2, &g_camera.reader[id]);
    if (ms != AMEDIA_OK || !g_camera.reader[id]) {
        LOGE("start: AImageReader_new(%d) failed: %d", id, ms);
        cam_stop_locked(id);
        return;
    }
    g_camera.img_listener[id].context = (void *)(intptr_t)id;
    g_camera.img_listener[id].onImageAvailable = on_frame_available;
    AImageReader_setImageListener(g_camera.reader[id], &g_camera.img_listener[id]);
    if (AImageReader_getWindow(g_camera.reader[id], &g_camera.window[id]) != AMEDIA_OK
            || !g_camera.window[id]) {
        LOGE("start: AImageReader_getWindow(%d) failed", id);
        cam_stop_locked(id);
        return;
    }

    /* 2. Open the device (synchronous; callbacks are for later disconnect/error). */
    g_camera.dev_cb[id].context = (void *)(intptr_t)id;
    g_camera.dev_cb[id].onDisconnected = on_device_disconnected;
    g_camera.dev_cb[id].onError = on_device_error;
    camera_status_t cs = ACameraManager_openCamera(g_camera.cam_mgr, g_camera.cam_ids[id],
                                                   &g_camera.dev_cb[id],
                                                   &g_camera.device[id]);
    if (cs != ACAMERA_OK || !g_camera.device[id]) {
        LOGE("start: openCamera(%d id=%s) failed: %d", id, g_camera.cam_ids[id], cs);
        cam_stop_locked(id);
        return;
    }

    /* 3. Capture session with the reader window as the only output. */
    if (ACaptureSessionOutputContainer_create(&g_camera.out_container[id]) != ACAMERA_OK ||
        ACaptureSessionOutput_create(g_camera.window[id], &g_camera.session_output[id]) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(g_camera.out_container[id],
                                           g_camera.session_output[id]) != ACAMERA_OK) {
        LOGE("start: session output setup(%d) failed", id);
        cam_stop_locked(id);
        return;
    }
    g_camera.ses_cb[id].context = (void *)(intptr_t)id;
    g_camera.ses_cb[id].onClosed = on_session_closed;
    g_camera.ses_cb[id].onReady = on_session_ready;
    g_camera.ses_cb[id].onActive = on_session_active;
    cs = ACameraDevice_createCaptureSession(g_camera.device[id], g_camera.out_container[id],
                                            &g_camera.ses_cb[id], &g_camera.session[id]);
    if (cs != ACAMERA_OK || !g_camera.session[id]) {
        LOGE("start: createCaptureSession(%d) failed: %d", id, cs);
        cam_stop_locked(id);
        return;
    }

    /* 4. Repeating TEMPLATE_RECORD request targeting the reader window. */
    if (ACameraDevice_createCaptureRequest(g_camera.device[id], TEMPLATE_RECORD,
                                           &g_camera.request[id]) != ACAMERA_OK ||
        ACameraOutputTarget_create(g_camera.window[id], &g_camera.out_target[id]) != ACAMERA_OK ||
        ACaptureRequest_addTarget(g_camera.request[id], g_camera.out_target[id]) != ACAMERA_OK) {
        LOGE("start: capture request setup(%d) failed", id);
        cam_stop_locked(id);
        return;
    }
    uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;
    uint8_t awbMode = ACAMERA_CONTROL_AWB_MODE_AUTO;
    uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
    ACaptureRequest_setEntry_u8(g_camera.request[id], ACAMERA_CONTROL_AE_MODE, 1, &aeMode);
    ACaptureRequest_setEntry_u8(g_camera.request[id], ACAMERA_CONTROL_AWB_MODE, 1, &awbMode);
    ACaptureRequest_setEntry_u8(g_camera.request[id], ACAMERA_CONTROL_AF_MODE, 1, &afMode);

    cs = ACameraCaptureSession_setRepeatingRequest(g_camera.session[id], NULL, 1,
                                                   &g_camera.request[id], NULL);
    if (cs != ACAMERA_OK) {
        LOGE("start: setRepeatingRequest(%d) failed: %d", id, cs);
        cam_stop_locked(id);
        return;
    }

    g_camera.cur_slot[id] = 0;
    LOGI("start: camera %d started %dx%d", id, w, h);
}

/* Tear down the capture pipeline for one camera. Must hold cap_lock[id]. */
static void cam_stop_locked(int id)
{
    if (g_camera.session[id]) {
        ACameraCaptureSession_stopRepeating(g_camera.session[id]);
        ACameraCaptureSession_close(g_camera.session[id]);
        g_camera.session[id] = NULL;
    }
    if (g_camera.request[id]) {
        ACaptureRequest_free(g_camera.request[id]);
        g_camera.request[id] = NULL;
    }
    if (g_camera.out_target[id]) {
        ACameraOutputTarget_free(g_camera.out_target[id]);
        g_camera.out_target[id] = NULL;
    }
    if (g_camera.session_output[id]) {
        ACaptureSessionOutput_free(g_camera.session_output[id]);
        g_camera.session_output[id] = NULL;
    }
    if (g_camera.out_container[id]) {
        ACaptureSessionOutputContainer_free(g_camera.out_container[id]);
        g_camera.out_container[id] = NULL;
    }
    if (g_camera.reader[id]) {
        /* Deleting the reader invalidates the window it vended. */
        AImageReader_delete(g_camera.reader[id]);
        g_camera.reader[id] = NULL;
        g_camera.window[id] = NULL;
    }
    if (g_camera.device[id]) {
        ACameraDevice_close(g_camera.device[id]);
        g_camera.device[id] = NULL;
    }
}

static void cam_start_recording(int id, int w, int h)
{
    if (id < 0 || id >= g_camera.num_cameras)
        return;
    pthread_mutex_lock(&g_camera.cap_lock[id]);
    cam_start_locked(id, w, h);
    pthread_mutex_unlock(&g_camera.cap_lock[id]);
}

static void cam_stop_recording(int id)
{
    if (id < 0 || id >= g_camera.num_cameras)
        return;
    pthread_mutex_lock(&g_camera.cap_lock[id]);
    cam_stop_locked(id);
    pthread_mutex_unlock(&g_camera.cap_lock[id]);
}

/* --- control channel (ctrl_fd) message handling --- */

static void ctrl_handle_msg(const struct camera_ctrl_msg *hdr, const uint8_t *payload)
{
    switch (hdr->type) {
    case CAMERA_CTRL_GET_INFO: {
        _Alignas(uint16_t) uint8_t reply[sizeof(struct camera_ctrl_msg) + 1 + MAX_CAMERAS * 4];
        struct camera_ctrl_msg *rh = (struct camera_ctrl_msg *)reply;
        rh->type = CAMERA_CTRL_INFO_REPLY;
        rh->reserved = 0;
        uint8_t *pl = reply + sizeof(struct camera_ctrl_msg);
        int n = g_camera.num_cameras;
        pl[0] = (uint8_t)n;
        size_t off = 1;
        for (int i = 0; i < n; i++) {
            uint16_t w16 = (uint16_t)g_camera.max_w[i];
            uint16_t h16 = (uint16_t)g_camera.max_h[i];
            memcpy(pl + off, &w16, sizeof(w16)); off += sizeof(w16);
            memcpy(pl + off, &h16, sizeof(h16)); off += sizeof(h16);
        }
        rh->len = (uint16_t)off;
        if (send(g_camera.ctrl_local, reply, sizeof(struct camera_ctrl_msg) + off,
                 MSG_NOSIGNAL) < 0)
            LOGE("ctrl: INFO_REPLY send failed: %s", strerror(errno));
        break;
    }
    case CAMERA_CTRL_START_RECORD: {
        if (hdr->len < 5) {
            LOGE("ctrl: START_RECORD short payload (%u)", hdr->len);
            break;
        }
        uint8_t id = payload[0];
        uint16_t w, h;
        memcpy(&w, payload + 1, sizeof(w));
        memcpy(&h, payload + 3, sizeof(h));
        if (id >= g_camera.num_cameras) {
            LOGE("ctrl: START_RECORD bad id %u", id);
            break;
        }
        LOGI("ctrl: START_RECORD cam=%u %ux%u", id, w, h);
        cam_start_recording(id, w, h);
        break;
    }
    case CAMERA_CTRL_STOP_RECORD: {
        if (hdr->len < 1) {
            LOGE("ctrl: STOP_RECORD short payload");
            break;
        }
        uint8_t id = payload[0];
        if (id >= g_camera.num_cameras)
            break;
        LOGI("ctrl: STOP_RECORD cam=%u", id);
        cam_stop_recording(id);
        break;
    }
    default:
        LOGE("ctrl: unknown msg type 0x%02x", hdr->type);
        break;
    }
}

/* --- stream channel (stream_fd[i]) message handling --- */

static void stream_handle_msg(int cam, const struct cam_stream_msg *m)
{
    switch (m->type) {
    case CAM_STREAM_GET_SHM:
        /* Producer wants the shm fd: offer the pre-created region. */
        send_stream(g_camera.stream_local[cam], CAM_STREAM_SHM_OFFER, 0, 0,
                    (uint32_t)g_camera.slot_bytes[cam], 0, g_camera.shm_fd[cam]);
        LOGI("stream cam=%d: offered shm (%zu B/slot)", cam, g_camera.slot_bytes[cam]);
        break;
    case CAM_STREAM_DONE: {
        uint8_t s = m->slot;
        if (s >= CAMERA_SLOTS)
            break;
        pthread_mutex_lock(&g_camera.slot_lock[cam]);
        g_camera.slot_free[cam][s] = true;
        pthread_cond_broadcast(&g_camera.slot_cond[cam]);
        pthread_mutex_unlock(&g_camera.slot_lock[cam]);
        break;
    }
    default:
        LOGE("stream cam=%d: unknown msg type %u", cam, m->type);
        break;
    }
}

/* --- I/O thread: polls ctrl_local + every stream_local for incoming messages --- */

static void *io_thread_func(void *arg)
{
    (void)arg;
    LOGI("io thread started");

    const int n = g_camera.num_cameras;
    struct pollfd pfds[1 + MAX_CAMERAS];

    while (g_camera.running) {
        pfds[0].fd = g_camera.ctrl_local;
        pfds[0].events = POLLIN;
        for (int i = 0; i < n; i++) {
            pfds[1 + i].fd = g_camera.stream_local[i];
            pfds[1 + i].events = POLLIN;
        }

        int r = poll(pfds, 1 + n, 500);
        if (r <= 0)
            continue;

        if (pfds[0].revents & POLLIN) {
            struct camera_ctrl_msg hdr;
            if (read_full(g_camera.ctrl_local, &hdr, sizeof(hdr)) == 0) {
                uint8_t payload[256];
                uint16_t len = hdr.len;
                if (len > sizeof(payload))
                    len = sizeof(payload);
                if (len == 0 || read_full(g_camera.ctrl_local, payload, len) == 0)
                    ctrl_handle_msg(&hdr, payload);
            } else {
                usleep(50000);   /* real read error: back off rather than spin */
            }
        }

        for (int i = 0; i < n; i++) {
            if (!(pfds[1 + i].revents & POLLIN))
                continue;
            struct cam_stream_msg m;
            ssize_t got = recv(g_camera.stream_local[i], &m, sizeof(m), 0);
            if (got == (ssize_t)sizeof(m))
                stream_handle_msg(i, &m);
        }
    }

    LOGI("io thread stopped");
    return NULL;
}

/* --- JNI: camera service lifecycle (called from CameraServices, main thread) --- */

/* Create the camera service's persistent fds and control thread. Idempotent.
 * Gated by the settings toggle on the Java side; once ready, do_connect()
 * registers SERVICE_TYPE_CAMERA so the producer can request it. The activity arg
 * is unused (the NDK needs no Context); the CAMERA permission is requested in Java. */
JNIEXPORT void JNICALL
Java_com_anland_termux_CameraServices_nativeInitCameraService(
    JNIEnv *env, jclass clazz, jobject activity)
{
    (void)env;
    (void)clazz;
    (void)activity;
    camera_service_init();
}

JNIEXPORT void JNICALL
Java_com_anland_termux_CameraServices_nativeDestroyCameraService(
    JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    camera_service_destroy();
}

/* --- service_info callbacks --- */

struct resources camera_allocate_resource(uint32_t *args)
{
    (void)args;
    struct resources res = {
        .service_type = SERVICE_TYPE_CAMERA,
        .type = -1,
        .num = 0,
        .fds = NULL,
    };
    if (!g_camera.ready)
        return res;

    res.type = 0;
    res.num = (uint32_t)(1 + g_camera.num_cameras);
    res.fds = g_camera.alloc_fds;
    LOGI("allocate_resource: %u fds (1 ctrl + %d stream)",
         res.num, g_camera.num_cameras);
    return res;
}

void camera_free_resource(struct resources res)
{
    (void)res;
    if (!g_camera.ready)
        return;
    LOGI("free_resource: stopping all recording");
    for (int i = 0; i < g_camera.num_cameras; i++)
        cam_stop_recording(i);

    /* Producer is gone: its DONEs will never come. Release every slot so a fresh
     * producer/analyzer cycle isn't wedged waiting on a stale claim. */
    for (int i = 0; i < g_camera.num_cameras; i++) {
        pthread_mutex_lock(&g_camera.slot_lock[i]);
        for (int s = 0; s < CAMERA_SLOTS; s++)
            g_camera.slot_free[i][s] = true;
        pthread_cond_broadcast(&g_camera.slot_cond[i]);
        pthread_mutex_unlock(&g_camera.slot_lock[i]);
    }
}

/* --- lifecycle --- */

bool camera_service_is_ready(void)
{
    return g_camera.ready;
}

/* Discover cameras via the NDK manager and cache the max YUV_420_888 output size of
 * each. Returns the camera count (>=0, capped at MAX_CAMERAS). */
static int discover_cameras(void)
{
    g_camera.cam_mgr = ACameraManager_create();
    if (!g_camera.cam_mgr) {
        LOGE("init: ACameraManager_create failed");
        return 0;
    }

    ACameraIdList *idList = NULL;
    if (ACameraManager_getCameraIdList(g_camera.cam_mgr, &idList) != ACAMERA_OK || !idList) {
        LOGE("init: getCameraIdList failed");
        return 0;
    }

    int n = idList->numCameras;
    if (n < 0) n = 0;
    if (n > MAX_CAMERAS) n = MAX_CAMERAS;

    for (int i = 0; i < n; i++) {
        g_camera.cam_ids[i] = strdup(idList->cameraIds[i]);

        ACameraMetadata *meta = NULL;
        if (ACameraManager_getCameraCharacteristics(g_camera.cam_mgr,
                idList->cameraIds[i], &meta) != ACAMERA_OK || !meta)
            continue;

        ACameraMetadata_const_entry entry;
        if (ACameraMetadata_getConstEntry(meta,
                ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) == ACAMERA_OK) {
            long bestArea = 0;
            /* tuples of 4: format, width, height, input(1)/output(0) */
            for (uint32_t k = 0; k + 3 < entry.count; k += 4) {
                int32_t fmt = entry.data.i32[k];
                int32_t w   = entry.data.i32[k + 1];
                int32_t h   = entry.data.i32[k + 2];
                int32_t io  = entry.data.i32[k + 3];
                if (fmt != AIMAGE_FORMAT_YUV_420_888 ||
                    io  != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)
                    continue;
                long area = (long)w * h;
                if (area > bestArea) {
                    bestArea = area;
                    g_camera.max_w[i] = w;
                    g_camera.max_h[i] = h;
                }
            }
        }
        ACameraMetadata_free(meta);
    }

    {
        char sb[256];
        int off = snprintf(sb, sizeof(sb), "init: cameras=%d", n);
        for (int i = 0; i < n && off < (int)sizeof(sb); i++)
            off += snprintf(sb + off, sizeof(sb) - off, " [%d id=%s max=%dx%d]",
                            i, g_camera.cam_ids[i] ? g_camera.cam_ids[i] : "?",
                            g_camera.max_w[i], g_camera.max_h[i]);
        LOGI("%s", sb);
    }

    ACameraManager_deleteCameraIdList(idList);
    return n;
}

/* Create the ashmem region for one camera (sized for its max resolution). Returns 0/-1. */
static int create_camera_shm(int i)
{
    int w = g_camera.max_w[i];
    int h = g_camera.max_h[i];
    size_t slot_bytes = (w > 0 && h > 0) ? (size_t)w * h * 3 / 2 : CAM_FALLBACK_BYTES;
    g_camera.slot_bytes[i] = slot_bytes;

    int fd = ASharedMemory_create("anland-camera", CAMERA_SLOTS * slot_bytes);
    if (fd < 0) {
        LOGE("init: ASharedMemory_create[%d] failed", i);
        return -1;
    }
    g_camera.shm_fd[i] = fd;

    void *p = mmap(NULL, CAMERA_SLOTS * slot_bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        LOGE("init: mmap shm[%d] failed: %s", i, strerror(errno));
        return -1;
    }
    g_camera.shm_ptr[i] = p;

    for (int s = 0; s < CAMERA_SLOTS; s++)
        g_camera.slot_free[i][s] = true;
    pthread_mutex_init(&g_camera.slot_lock[i], NULL);
    pthread_cond_init(&g_camera.slot_cond[i], NULL);
    pthread_mutex_init(&g_camera.cap_lock[i], NULL);
    return 0;
}

int camera_service_init(void)
{
    if (g_camera.ready)
        return 0;                    /* idempotent */

    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.ctrl_local = g_camera.ctrl_remote = -1;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        g_camera.stream_local[i] = g_camera.stream_remote[i] = -1;
        g_camera.shm_fd[i] = -1;
    }

    int num = discover_cameras();
    g_camera.num_cameras = num;
    LOGI("init: %d camera(s)", num);

    /* Shared control pair (SOCK_STREAM: variable-length ctrl msgs). */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        LOGE("init: ctrl socketpair failed: %s", strerror(errno));
        goto fail;
    }
    g_camera.ctrl_local  = sv[0];
    g_camera.ctrl_remote = sv[1];
    g_camera.alloc_fds[0] = g_camera.ctrl_remote;

    /* Per camera: a SEQPACKET stream control pair + an ashmem double buffer. */
    for (int i = 0; i < num; i++) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
            LOGE("init: stream socketpair[%d] failed: %s", i, strerror(errno));
            goto fail;
        }
        g_camera.stream_local[i]  = sv[0];
        g_camera.stream_remote[i] = sv[1];
        g_camera.alloc_fds[1 + i] = g_camera.stream_remote[i];

        if (create_camera_shm(i) < 0)
            goto fail;
    }

    g_camera.running = true;
    if (pthread_create(&g_camera.io_thread, NULL, io_thread_func, NULL) != 0) {
        LOGE("init: io thread create failed");
        g_camera.running = false;
        goto fail;
    }

    g_camera.ready = true;
    LOGI("camera service initialised");
    return 0;

fail:
    if (g_camera.ctrl_local >= 0)  close(g_camera.ctrl_local);
    if (g_camera.ctrl_remote >= 0) close(g_camera.ctrl_remote);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (g_camera.stream_local[i] >= 0)  close(g_camera.stream_local[i]);
        if (g_camera.stream_remote[i] >= 0) close(g_camera.stream_remote[i]);
        if (g_camera.shm_ptr[i])
            munmap(g_camera.shm_ptr[i], CAMERA_SLOTS * g_camera.slot_bytes[i]);
        if (g_camera.shm_fd[i] >= 0) close(g_camera.shm_fd[i]);
        free(g_camera.cam_ids[i]);
    }
    if (g_camera.cam_mgr) {
        ACameraManager_delete(g_camera.cam_mgr);
        g_camera.cam_mgr = NULL;
    }
    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.ctrl_local = g_camera.ctrl_remote = -1;
    return -1;
}

void camera_service_destroy(void)
{
    if (!g_camera.ready)
        return;
    LOGI("camera service destroying");

    g_camera.ready = false;
    g_camera.running = false;
    pthread_join(g_camera.io_thread, NULL);

    for (int i = 0; i < g_camera.num_cameras; i++)
        cam_stop_recording(i);

    if (g_camera.ctrl_local >= 0)  close(g_camera.ctrl_local);
    if (g_camera.ctrl_remote >= 0) close(g_camera.ctrl_remote);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (g_camera.stream_local[i] >= 0)  close(g_camera.stream_local[i]);
        if (g_camera.stream_remote[i] >= 0) close(g_camera.stream_remote[i]);
        if (g_camera.shm_ptr[i])
            munmap(g_camera.shm_ptr[i], CAMERA_SLOTS * g_camera.slot_bytes[i]);
        if (g_camera.shm_fd[i] >= 0) close(g_camera.shm_fd[i]);
        free(g_camera.cam_ids[i]);
        if (g_camera.num_cameras > i) {
            pthread_mutex_destroy(&g_camera.slot_lock[i]);
            pthread_cond_destroy(&g_camera.slot_cond[i]);
            pthread_mutex_destroy(&g_camera.cap_lock[i]);
        }
    }

    if (g_camera.cam_mgr) {
        ACameraManager_delete(g_camera.cam_mgr);
        g_camera.cam_mgr = NULL;
    }

    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.ctrl_local = g_camera.ctrl_remote = -1;
}

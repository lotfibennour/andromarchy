/*
 * fdhelper — a tiny root helper for the "connect with root" mode.
 *
 * The Android app cannot connect() directly to the daemon's unix socket when it
 * lives in a root-only location (e.g. /data/local/tmp owned by root, blocked by
 * SELinux/DAC for untrusted_app). Instead the app launches this helper through
 * `su -c`, the helper (running as root) connects to the daemon socket, then
 * hands the connected fd back to the app over a bridge unix socket the app is
 * listening on (SCM_RIGHTS). Once the fd is in the app's process it can drive
 * the daemon exactly as if it had connected itself.
 *
 *   usage: libfdhelper.so <daemon_socket_path> <bridge_socket_path>
 *
 * It is shipped inside the APK as lib*.so so Android extracts it into the app's
 * nativeLibraryDir with execute permission.
 */
#include <android/log.h>
#include <unistd.h>

#include "socket_utils.h"

#define TAG "AnlandFdHelper"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

int main(int argc, char **argv)
{
    if (argc < 3) {
        LOGE("usage: %s <daemon_socket> <bridge_socket>", argv[0]);
        return 1;
    }
    const char *daemon_sock = argv[1];
    const char *bridge_sock = argv[2];

    int dfd = connect_unix(daemon_sock);
    if (dfd < 0) {
        LOGE("connect to daemon socket '%s' failed", daemon_sock);
        return 2;
    }

    int bfd = connect_unix(bridge_sock);
    if (bfd < 0) {
        LOGE("connect to bridge socket '%s' failed", bridge_sock);
        close(dfd);
        return 3;
    }

    /* The one byte is just a non-empty payload to carry the ancillary fd. */
    char marker = 'F';
    if (send_fds(bfd, &marker, 1, &dfd, 1) < 0) {
        LOGE("send_fds failed");
        close(bfd);
        close(dfd);
        return 4;
    }

    LOGI("passed daemon fd to app");
    close(bfd);   /* fd already queued in the kernel; safe to close */
    close(dfd);
    return 0;
}

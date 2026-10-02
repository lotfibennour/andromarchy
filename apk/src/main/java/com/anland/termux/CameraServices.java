package com.anland.termux;

import android.app.Activity;

/**
 * Thin JNI shell for the camera service.
 *
 * All camera work — discovery, opening devices, capture sessions, and per-frame
 * YUV delivery into the shared-memory slots — now lives in camera_service.c using
 * the Camera2 NDK (libcamera2ndk / libmediandk).  The previous Camera2 Java
 * implementation and its per-frame JNI round-trips (nativeAwaitSlotFree /
 * nativePackFrame / nativeFrameReady) are gone.
 *
 * This class exists only to expose the two lifecycle entry points so the native
 * service can be created/destroyed from {@code MainActivity}.  The {@code Activity}
 * argument is no longer used by native (the NDK needs no Context) but is kept so
 * MainActivity's call sites are unchanged; the CAMERA runtime permission is still
 * requested on the Java side before init is called.
 */
public class CameraServices {
    static native void nativeInitCameraService(Activity activity);
    static native void nativeDestroyCameraService();
}

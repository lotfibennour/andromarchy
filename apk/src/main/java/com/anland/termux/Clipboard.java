package com.anland.termux;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;

import java.nio.charset.StandardCharsets;

/**
 * Bridges the Android system clipboard with the remote compositor.
 *
 * An instance is handed to {@code Native.nativeStart} as the callback target, so
 * the native event thread invokes {@link #nativeSetClipboardText},
 * {@link #nativeClipboardSync} and {@link #nativeClipListening} directly on it
 * (resolved by name via JNI) — those three names and signatures must stay in sync
 * with {@code jni/native_consumer.c}.
 */
public final class Clipboard {
    private final Context context;
    // Last text we pushed either way; used to skip the clipListener echo.
    private String mLastSentClip = null;
    private boolean mClipListening = false;

    Clipboard(Context context) {
        this.context = context;
    }

    private final ClipboardManager.OnPrimaryClipChangedListener clipListener =
        () -> pushClipboard();

    // Called from the native event thread to set clipboard text on Android.
    public void nativeSetClipboardText(String text) {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm != null) {
            mLastSentClip = text;  // 记录，clipListener 回环时会比对跳过
            cm.setPrimaryClip(ClipData.newPlainText("anland", text));
        }
    }

    // Called from native C on exit_fallback to send initial clipboard sync.
    public void nativeClipboardSync() {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence text = clip.getItemAt(0).getText();
            if (text != null) {
                mLastSentClip = text.toString();
                Native.nativeSendClipboard(text.toString().getBytes(StandardCharsets.UTF_8));
            }
        }
    }

    // Called from native C: true = register clip listener, false = unregister.
    public void nativeClipListening(boolean enable) {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm == null) return;
        if (enable) {
            if (mClipListening) return;  // already registered
            cm.addPrimaryClipChangedListener(clipListener);
            mClipListening = true;
        } else {
            if (!mClipListening) return;  // not registered
            cm.removePrimaryClipChangedListener(clipListener);
            mClipListening = false;
        }
    }

    // Push clipboard only if content actually changed.
    public void pushClipboard() {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence text = clip.getItemAt(0).getText();
            if (text != null) {
                String clipText = text.toString();
                if (!clipText.equals(mLastSentClip)) {
                    mLastSentClip = clipText;
                    Native.nativeSendClipboard(clipText.getBytes(StandardCharsets.UTF_8));
                }
            }
        }
    }
}

package com.anland.termux;

import android.content.Context;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

import java.io.File;
import java.io.FileDescriptor;

/**
 * Long touch press (one finger held still): on release, asks the session's andromarchy-longpress script for the
 * edit menu (Copy, Paste...) through the FIFO run/longpress. Only observes: the touch still reaches the app, so apps
 * with their own long-press menu (browsers, which the script skips) keep it.
 */
final class LongPress {
    private static final long DELAY_MS = 500;

    private final File fifo;
    private final int slop;
    private long downTime = -1;
    private float downX, downY;

    LongPress(Context ctx) {
        fifo = new File(SessionService.runDir(ctx), "longpress");
        slop = ViewConfiguration.get(ctx).getScaledTouchSlop();
    }

    void onTouch(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downTime = SystemClock.uptimeMillis();
                downX = e.getX();
                downY = e.getY();
                break;
            case MotionEvent.ACTION_MOVE:
                if (downTime >= 0 && Math.hypot(e.getX() - downX, e.getY() - downY) > slop)
                    downTime = -1; // a drag or a selection, not a press
                break;
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_CANCEL:
                downTime = -1;
                break;
            case MotionEvent.ACTION_UP:
                if (downTime >= 0 && SystemClock.uptimeMillis() - downTime >= DELAY_MS)
                    request();
                downTime = -1;
                break;
        }
    }

    private void request() {
        new Thread(() -> {
            try {
                // Non-blocking: without a reader (session not started) open fails with ENXIO instead of hanging.
                FileDescriptor fd = Os.open(fifo.getPath(), OsConstants.O_WRONLY | OsConstants.O_NONBLOCK, 0);
                try {
                    byte[] line = "menu\n".getBytes();
                    Os.write(fd, line, 0, line.length);
                } finally {
                    Os.close(fd);
                }
            } catch (ErrnoException | java.io.InterruptedIOException ignored) { // no session yet: nothing to show
            }
        }).start();
    }
}

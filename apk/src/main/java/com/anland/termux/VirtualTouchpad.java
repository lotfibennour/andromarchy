package com.anland.termux;

import android.content.Context;
import android.graphics.Point;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import android.view.WindowManager;

/**
 * Laptop-style virtual touchpad: interprets finger gestures on the surface as
 * relative mouse motion, taps/clicks, long-press drag and two-finger scroll,
 * forwarding them to the remote through {@link Native}.
 *
 * Self-contained state machine — the host routes non-mouse touches here (see
 * MainActivity.onTouchEvent) when touchpad mode is on, pushes the acceleration
 * preference via {@link #setAccelStrength}, and calls {@link #onSurfaceChanged}
 * when the surface is resized.
 */
public final class VirtualTouchpad {

    // 状态机
    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE_FINGER = 1;
    private static final int STATE_TWO_FINGER = 2;
    private static final int STATE_DRAGGING = 3;
    private int currentState = STATE_IDLE;

    private float lastX1, lastY1;
    private float startX1, startY1;
    private float lastX2, lastY2;
    private long downTime1;
    private final float touchSlop;

    private boolean isSingleTapCandidate = false;
    private boolean isTwoFingerTapCandidate = false;
    private boolean isDraggingActive = false;

    private long lastTapTime = 0;
    private float lastTapX, lastTapY;
    private boolean isDoubleTapPending = false;

    private static final long TOUCH_LONG_PRESS_TIMEOUT = 500;
    private boolean hasLongPressed = false;
    private boolean isLongPressPossible = false;
    private boolean isMultiFinger = false;

    // 鼠标位置（相对模式）
    private float mouseX = 0;
    private float mouseY = 0;
    private int screenWidth = 1920;
    private int screenHeight = 1080;

    private float mouseAccelStrength = 1.0f; // 加速度强度，0.5 ~ 10.0

    // ===== 调整后的平滑/抗抖动参数（更灵敏、更连续） =====
    private static final float DEAD_ZONE = 0.3f;          // 死区从 0.5 降到 0.3
    private static final float SMOOTHING_FACTOR = 0.45f;   // 提高响应速度
    private static final float ACCUMULATED_THRESHOLD = 0.1f; // 从 0.8 大幅降低，让移动更连续

    private float smoothedDx = 0f;
    private float smoothedDy = 0f;
    private float accumulatedX = 0f;
    private float accumulatedY = 0f;
    private boolean smoothInitialized = false;

    private final Context context;

    VirtualTouchpad(Context context) {
        this.context = context;
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        updateScreenSize();
        mouseX = screenWidth / 2f;
        mouseY = screenHeight / 2f;
    }

    /** Set the acceleration strength (clamped to 0.5 ~ 10.0). */
    void setAccelStrength(float strength) {
        mouseAccelStrength = Math.max(0.5f, Math.min(10.0f, strength));
    }

    /** Re-read screen size and re-anchor the cursor after a surface resize. */
    void onSurfaceChanged() {
        updateScreenSize();
        mouseX = clamp(mouseX, 0, screenWidth);
        mouseY = clamp(mouseY, 0, screenHeight);
        resetSmoothing();
    }

    // ==================== 触摸板手势及辅助方法 ====================
    boolean onTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();

        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                float x = event.getX();
                float y = event.getY();
                startX1 = lastX1 = x;
                startY1 = lastY1 = y;
                downTime1 = event.getEventTime();
                hasLongPressed = false;
                isLongPressPossible = true;
                isSingleTapCandidate = true;
                isTwoFingerTapCandidate = false;
                isDraggingActive = false;
                isMultiFinger = false;
                currentState = STATE_ONE_FINGER;
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                isMultiFinger = true;
                isSingleTapCandidate = false;
                isLongPressPossible = false;
                if (currentState == STATE_DRAGGING) {
                    Native.nativeSendMouseButton(0x110, false);
                    isDraggingActive = false;
                }
                if (pointerCount == 2) {
                    currentState = STATE_TWO_FINGER;
                    isTwoFingerTapCandidate = true;
                    lastX1 = event.getX(0);
                    lastY1 = event.getY(0);
                    lastX2 = event.getX(1);
                    lastY2 = event.getY(1);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (pointerCount == 1 && !isMultiFinger) {
                    float x = event.getX();
                    float y = event.getY();
                    float rawDx = x - lastX1;
                    float rawDy = y - lastY1;
                    float dist = (float) Math.hypot(x - startX1, y - startY1);

                    if (dist > touchSlop) {
                        isLongPressPossible = false;
                        isSingleTapCandidate = false;
                    }

                    if (isLongPressPossible && !hasLongPressed &&
                            (event.getEventTime() - downTime1) >= TOUCH_LONG_PRESS_TIMEOUT) {
                        hasLongPressed = true;
                        currentState = STATE_DRAGGING;
                        isDraggingActive = true;
                        Native.nativeSendMouseButton(0x110, true);
                        mouseX = clamp(mouseX, 0, screenWidth);
                        mouseY = clamp(mouseY, 0, screenHeight);
                        Native.nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                        resetSmoothing();
                        break;
                    }

                    float[] smoothed = applySmoothing(rawDx, rawDy);
                    float smoothDx = smoothed[0];
                    float smoothDy = smoothed[1];

                    if (smoothDx != 0f || smoothDy != 0f) {
                        // 计算移动距离（平滑后的欧式距离）
                        float distance = (float) Math.hypot(smoothDx, smoothDy);

                        // 改进的加速度曲线：以 10px 为参考阈值，使小位移也能获得明显加速
                        float speedFactor = distance / 10.0f;
                        // 使用 sigmoid-like 曲线：scale = 1 + (strength - 1) * (speed / (1 + speed))
                        float dynamicScale = 1.0f + (mouseAccelStrength - 1.0f) * (speedFactor / (1.0f + speedFactor));
                        // 限制范围，防止失控（最大不超过 10 倍）
                        dynamicScale = Math.max(0.3f, Math.min(10.0f, dynamicScale));

                        float moveX = smoothDx * dynamicScale;
                        float moveY = smoothDy * dynamicScale;
                        mouseX = clamp(mouseX + moveX, 0, screenWidth);
                        mouseY = clamp(mouseY + moveY, 0, screenHeight);
                        Native.nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                    }

                    lastX1 = x;
                    lastY1 = y;

                } else if (pointerCount == 2) {
                    if (currentState == STATE_TWO_FINGER) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float x2 = event.getX(1);
                        float y2 = event.getY(1);
                        float avgDx = ((x1 - lastX1) + (x2 - lastX2)) / 2;
                        float avgDy = ((y1 - lastY1) + (y2 - lastY2)) / 2;

                        if (Math.abs(avgDx) > 1 || Math.abs(avgDy) > 1) {
                            isTwoFingerTapCandidate = false;
                            if (Math.abs(avgDy) > Math.abs(avgDx) * 0.5) {
                                Native.nativeSendMouseScroll(0, -avgDy * 0.5f);
                            }
                            if (Math.abs(avgDx) > Math.abs(avgDy) * 0.5) {
                                Native.nativeSendMouseScroll(1, avgDx * 0.5f);
                            }
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        }
                    }
                }
                break;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int remaining = pointerCount - 1;
                if (remaining == 1) {
                    isMultiFinger = false;
                    isSingleTapCandidate = false;
                    isLongPressPossible = false;
                    int idx = (event.getActionIndex() == 0) ? 1 : 0;
                    lastX1 = event.getX(idx);
                    lastY1 = event.getY(idx);
                    startX1 = lastX1;
                    startY1 = lastY1;
                    downTime1 = event.getEventTime();
                    hasLongPressed = false;
                    currentState = STATE_ONE_FINGER;
                    resetSmoothing();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                long duration = event.getEventTime() - downTime1;
                boolean isQuickTap = duration < 300;

                if (isDraggingActive) {
                    Native.nativeSendMouseButton(0x110, false);
                    isDraggingActive = false;
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (isTwoFingerTapCandidate && isQuickTap) {
                    Native.nativeSendMouseButton(0x111, true);
                    Native.nativeSendMouseButton(0x111, false);
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (currentState == STATE_ONE_FINGER && isSingleTapCandidate && isQuickTap) {
                    long gap = event.getEventTime() - lastTapTime;
                    float dist = (float) Math.hypot(lastX1 - lastTapX, lastY1 - lastTapY);
                    if (gap < 300 && dist < touchSlop && !isDoubleTapPending) {
                        isDoubleTapPending = true;
                        Native.nativeSendMouseButton(0x110, true);
                        Native.nativeSendMouseButton(0x110, false);
                        Native.nativeSendMouseButton(0x110, true);
                        Native.nativeSendMouseButton(0x110, false);
                        isDoubleTapPending = false;
                        lastTapTime = 0;
                    } else {
                        Native.nativeSendMouseButton(0x110, true);
                        Native.nativeSendMouseButton(0x110, false);
                        lastTapTime = event.getEventTime();
                        lastTapX = lastX1;
                        lastTapY = lastY1;
                        isDoubleTapPending = false;
                    }
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (isDraggingActive) {
                    Native.nativeSendMouseButton(0x110, false);
                    isDraggingActive = false;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
        }
        return true;
    }

    private void resetTouchpadState() {
        currentState = STATE_IDLE;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isDoubleTapPending = false;
        hasLongPressed = false;
        isDraggingActive = false;
        isLongPressPossible = false;
        isMultiFinger = false;
    }

    private void resetSmoothing() {
        smoothedDx = 0f;
        smoothedDy = 0f;
        accumulatedX = 0f;
        accumulatedY = 0f;
        smoothInitialized = false;
    }

    private float[] applySmoothing(float rawDx, float rawDy) {
        float deadDx = Math.abs(rawDx) < DEAD_ZONE ? 0f : rawDx;
        float deadDy = Math.abs(rawDy) < DEAD_ZONE ? 0f : rawDy;

        if (deadDx == 0f && deadDy == 0f) {
            return new float[]{0f, 0f};
        }

        if (!smoothInitialized) {
            smoothedDx = deadDx;
            smoothedDy = deadDy;
            smoothInitialized = true;
        } else {
            smoothedDx = SMOOTHING_FACTOR * deadDx + (1 - SMOOTHING_FACTOR) * smoothedDx;
            smoothedDy = SMOOTHING_FACTOR * deadDy + (1 - SMOOTHING_FACTOR) * smoothedDy;
        }

        // 累积阈值大幅降低，让移动更加连续
        accumulatedX += smoothedDx;
        accumulatedY += smoothedDy;

        float outX = 0f;
        float outY = 0f;
        if (Math.abs(accumulatedX) >= ACCUMULATED_THRESHOLD) {
            outX = accumulatedX;
            accumulatedX = 0f;
        }
        if (Math.abs(accumulatedY) >= ACCUMULATED_THRESHOLD) {
            outY = accumulatedY;
            accumulatedY = 0f;
        }
        return new float[]{outX, outY};
    }

    private float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }

    private void updateScreenSize() {
        Point size = new Point();
        WindowManager wm = context.getSystemService(WindowManager.class);
        if (wm != null) {
            wm.getDefaultDisplay().getSize(size);
            screenWidth = size.x;
            screenHeight = size.y;
        }
    }
}

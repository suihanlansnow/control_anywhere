package com.rc.remote;

import android.content.Context;
import android.graphics.Point;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;

/** 等比显示远程桌面，并把触控转换为远端像素坐标。 */
final class RemoteSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
    interface Listener {
        void onRightButtonModeChanged(boolean enabled);
    }

    private final H264Decoder decoder;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float touchSlop;
    private RelayClient client;
    private Listener listener;
    private volatile int remoteWidth;
    private volatile int remoteHeight;

    private int activePointerId;
    private float downX;
    private float downY;
    private float lastX;
    private float lastY;
    private float lastScrollY;
    private float scrollRemainder;
    private boolean dragging;
    private boolean leftDown;
    private boolean multiTouch;
    private boolean ignoreUntilAllUp;
    private boolean longPressFired;
    private boolean rightButtonMode;

    private final Runnable longPress = new Runnable() {
        @Override
        public void run() {
            RelayClient current = client;
            if (current == null || dragging || multiTouch || ignoreUntilAllUp) return;
            Point p = toRemote(lastX, lastY);
            current.sendMouseButton(2, true, p.x, p.y);
            current.sendMouseButton(2, false, p.x, p.y);
            longPressFired = true;
            performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        }
    };

    private final View.OnLayoutChangeListener containerLayoutListener =
            (view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) ->
                    fitToContainer();

    RemoteSurfaceView(Context context, H264Decoder.Listener decoderListener) {
        super(context);
        decoder = new H264Decoder(decoderListener);
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setKeepScreenOn(true);
    }

    void setClient(RelayClient client) {
        this.client = client;
    }

    void setListener(Listener listener) {
        this.listener = listener;
    }

    void setRightButtonMode(boolean enabled) {
        rightButtonMode = enabled;
        if (listener != null) listener.onRightButtonModeChanged(enabled);
    }

    boolean isRightButtonMode() {
        return rightButtonMode;
    }

    /** 由网络线程调用，只更新尺寸并投递布局，避免跨线程操作视图。 */
    void offerFrame(byte[] payload) {
        if (payload.length >= 32) {
            int width = Protocol.u16(payload, 20);
            int height = Protocol.u16(payload, 22);
            if (width > 0 && height > 0 && (width != remoteWidth || height != remoteHeight)) {
                remoteWidth = width;
                remoteHeight = height;
                post(this::fitToContainer);
            }
        }
        decoder.offer(payload);
    }

    void release() {
        handler.removeCallbacks(longPress);
        decoder.detach();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        decoder.attach(holder.getSurface());
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (getParent() instanceof View) {
            ((View) getParent()).addOnLayoutChangeListener(containerLayoutListener);
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        if (getParent() instanceof View) {
            ((View) getParent()).removeOnLayoutChangeListener(containerLayoutListener);
        }
        super.onDetachedFromWindow();
    }

    /** 让 Surface 按远程画面比例居中，避免解码器把画面拉伸到整个区域。 */
    private void fitToContainer() {
        if (remoteWidth <= 0 || remoteHeight <= 0) return;
        if (!(getParent() instanceof View)) return;
        View container = (View) getParent();
        int availableWidth = container.getWidth();
        int availableHeight = container.getHeight();
        if (availableWidth <= 0 || availableHeight <= 0) return;
        float scale = Math.min(availableWidth / (float) remoteWidth,
                availableHeight / (float) remoteHeight);
        int targetWidth = Math.max(1, Math.round(remoteWidth * scale));
        int targetHeight = Math.max(1, Math.round(remoteHeight * scale));
        ViewGroup.LayoutParams params = getLayoutParams();
        if (params != null && (params.width != targetWidth || params.height != targetHeight)) {
            params.width = targetWidth;
            params.height = targetHeight;
            setLayoutParams(params);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        handler.removeCallbacks(longPress);
        decoder.detach();
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        RelayClient current = client;
        if (current == null || remoteWidth <= 0 || remoteHeight <= 0) return true;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                requestFocus();
                getParent().requestDisallowInterceptTouchEvent(true);
                activePointerId = event.getPointerId(0);
                downX = lastX = event.getX();
                downY = lastY = event.getY();
                dragging = false;
                leftDown = false;
                multiTouch = false;
                ignoreUntilAllUp = false;
                longPressFired = false;
                scrollRemainder = 0;
                Point down = toRemote(downX, downY);
                current.sendMouseMove(down.x, down.y);
                handler.postDelayed(longPress, 550);
                return true;

            case MotionEvent.ACTION_POINTER_DOWN:
                if (event.getPointerCount() == 2) {
                    multiTouch = true;
                    ignoreUntilAllUp = true;
                    dragging = false;
                    handler.removeCallbacks(longPress);
                    if (leftDown) {
                        Point p = toRemote(lastX, lastY);
                        current.sendMouseButton(1, false, p.x, p.y);
                        leftDown = false;
                    }
                    lastScrollY = (event.getY(0) + event.getY(1)) * 0.5f;
                    scrollRemainder = 0;
                }
                return true;

            case MotionEvent.ACTION_MOVE:
                if (multiTouch && event.getPointerCount() >= 2) {
                    float y = (event.getY(0) + event.getY(1)) * 0.5f;
                    scrollRemainder += y - lastScrollY;
                    lastScrollY = y;
                    while (scrollRemainder >= 42) {
                        current.sendMouseWheel(0, 120);
                        scrollRemainder -= 42;
                    }
                    while (scrollRemainder <= -42) {
                        current.sendMouseWheel(0, -120);
                        scrollRemainder += 42;
                    }
                    return true;
                }
                int index = event.findPointerIndex(activePointerId);
                if (index < 0 || ignoreUntilAllUp) return true;
                lastX = event.getX(index);
                lastY = event.getY(index);
                float dx = lastX - downX;
                float dy = lastY - downY;
                if (!dragging && dx * dx + dy * dy > touchSlop * touchSlop) {
                    dragging = true;
                    handler.removeCallbacks(longPress);
                    Point start = toRemote(downX, downY);
                    current.sendMouseMove(start.x, start.y);
                    current.sendMouseButton(1, true, start.x, start.y);
                    leftDown = true;
                }
                if (dragging) {
                    Point p = toRemote(lastX, lastY);
                    current.sendMouseMove(p.x, p.y);
                }
                return true;

            case MotionEvent.ACTION_POINTER_UP:
                if (multiTouch && event.getPointerCount() <= 2) {
                    multiTouch = false;
                    scrollRemainder = 0;
                }
                return true;

            case MotionEvent.ACTION_UP:
                handler.removeCallbacks(longPress);
                lastX = event.getX();
                lastY = event.getY();
                if (!ignoreUntilAllUp && !longPressFired) {
                    Point p = toRemote(lastX, lastY);
                    if (dragging || leftDown) {
                        current.sendMouseMove(p.x, p.y);
                        if (leftDown) current.sendMouseButton(1, false, p.x, p.y);
                    } else {
                        int button = rightButtonMode ? 2 : 1;
                        current.sendMouseMove(p.x, p.y);
                        current.sendMouseButton(button, true, p.x, p.y);
                        current.sendMouseButton(button, false, p.x, p.y);
                        performClick();
                        if (rightButtonMode) setRightButtonMode(false);
                    }
                } else if (leftDown) {
                    Point p = toRemote(lastX, lastY);
                    current.sendMouseButton(1, false, p.x, p.y);
                }
                resetGesture();
                return true;

            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                if (leftDown) {
                    Point p = toRemote(lastX, lastY);
                    current.sendMouseButton(1, false, p.x, p.y);
                }
                resetGesture();
                return true;
            default:
                return true;
        }
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    private void resetGesture() {
        dragging = false;
        leftDown = false;
        multiTouch = false;
        ignoreUntilAllUp = false;
        longPressFired = false;
        scrollRemainder = 0;
    }

    private Point toRemote(float x, float y) {
        // MediaCodec 会把码流铺满 Surface，Surface 本身已按画面比例调整过，
        // 这里按线性缩放换算，避免引入黑边偏移导致点击落点错位。
        int remoteX = Math.round(x / Math.max(1, getWidth()) * remoteWidth);
        int remoteY = Math.round(y / Math.max(1, getHeight()) * remoteHeight);
        return new Point(clamp(remoteX, 0, remoteWidth - 1),
                clamp(remoteY, 0, remoteHeight - 1));
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }
}

package com.rc.remote;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;

/** Annex-B H.264 系统解码器，待解码队列深度始终为 1。 */
final class H264Decoder {
    interface Listener {
        void onFirstFrame();
        void onDecoderError(String message);
    }

    private static final class Frame {
        final byte[] payload;
        final int width;
        final int height;
        final int dataLength;
        final boolean keyframe;

        Frame(byte[] payload, int width, int height, int dataLength, boolean keyframe) {
            this.payload = payload;
            this.width = width;
            this.height = height;
            this.dataLength = dataLength;
            this.keyframe = keyframe;
        }
    }

    private final Object lock = new Object();
    private final Listener listener;
    private Surface surface;
    private Thread worker;
    private boolean running;
    private Frame pending;

    H264Decoder(Listener listener) {
        this.listener = listener;
    }

    void attach(Surface surface) {
        detach();
        synchronized (lock) {
            this.surface = surface;
            running = true;
            worker = new Thread(this::decodeLoop, "rc-h264");
            worker.setDaemon(true);
            worker.start();
        }
    }

    void detach() {
        Thread oldWorker;
        synchronized (lock) {
            running = false;
            surface = null;
            pending = null;
            oldWorker = worker;
            worker = null;
            lock.notifyAll();
        }
        if (oldWorker != null && oldWorker != Thread.currentThread()) {
            try {
                oldWorker.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    void offer(byte[] payload) {
        if (payload.length < 32 || Protocol.u8(payload, 24) != 1) return;
        int width = Protocol.u16(payload, 20);
        int height = Protocol.u16(payload, 22);
        int dataLength = Protocol.i32(payload, 28);
        if (width <= 0 || height <= 0 || dataLength <= 0 || dataLength > payload.length - 32) {
            return;
        }
        Frame frame = new Frame(payload, width, height, dataLength,
                (Protocol.u8(payload, 25) & 1) != 0);
        synchronized (lock) {
            if (!running || surface == null) return;
            pending = frame;
            lock.notifyAll();
        }
    }

    private void decodeLoop() {
        MediaCodec codec = null;
        int codecWidth = 0;
        int codecHeight = 0;
        boolean hasKeyframe = false;
        boolean rendered = false;
        long presentationTimeUs = 0;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        try {
            while (true) {
                Frame frame;
                Surface target;
                synchronized (lock) {
                    while (running && pending == null) lock.wait();
                    if (!running) break;
                    frame = pending;
                    pending = null;
                    target = surface;
                }
                if (target == null) continue;

                if (codec == null || codecWidth != frame.width || codecHeight != frame.height) {
                    if (codec != null) releaseCodec(codec);
                    codec = createCodec(target, frame.width, frame.height);
                    codecWidth = frame.width;
                    codecHeight = frame.height;
                    hasKeyframe = false;
                }
                if (!hasKeyframe) {
                    if (!frame.keyframe) continue;
                    hasKeyframe = true;
                }

                int inputIndex = codec.dequeueInputBuffer(10000);
                if (inputIndex < 0) continue;
                ByteBuffer input = codec.getInputBuffer(inputIndex);
                if (input == null) continue;
                input.clear();
                input.put(frame.payload, 32, frame.dataLength);
                presentationTimeUs += 33333;
                codec.queueInputBuffer(inputIndex, 0, frame.dataLength, presentationTimeUs,
                        frame.keyframe ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);

                while (running) {
                    int outputIndex = codec.dequeueOutputBuffer(info, 0);
                    if (outputIndex >= 0) {
                        // Surface 输出下 info.size 可能为 0，仍需渲染；
                        // 渲染与否由 releaseOutputBuffer 的第二个参数决定。
                        codec.releaseOutputBuffer(outputIndex, true);
                        if (!rendered) {
                            rendered = true;
                            listener.onFirstFrame();
                        }
                    } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        // Surface 输出无需读取格式中的像素数据。
                    } else {
                        break;
                    }
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (Exception e) {
            if (running) {
                listener.onDecoderError("H.264 解码失败：" + e.getClass().getSimpleName()
                        + (e.getMessage() == null ? "" : " " + e.getMessage()));
            }
        } finally {
            synchronized (lock) {
                running = false;
                pending = null;
                lock.notifyAll();
            }
            if (codec != null) releaseCodec(codec);
        }
    }

    private static MediaCodec createCodec(Surface surface, int width, int height) throws IOException {
        MediaCodec codec = MediaCodec.createDecoderByType("video/avc");
        try {
            MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
            codec.configure(format, surface, null, 0);
            codec.start();
            return codec;
        } catch (Exception e) {
            releaseCodec(codec);
            throw e;
        }
    }

    private static void releaseCodec(MediaCodec codec) {
        try {
            codec.stop();
        } catch (Exception ignored) {
        }
        try {
            codec.release();
        } catch (Exception ignored) {
        }
    }
}

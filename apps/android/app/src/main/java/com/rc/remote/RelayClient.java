package com.rc.remote;

import android.util.Base64;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketException;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** 单条 TCP 连接完成注册、鉴权、视频接收与输入转发。 */
final class RelayClient {
    interface Listener {
        void onStatus(String status);
        void onVideoFrame(byte[] payload);
        void onDisconnected(String reason, boolean intentional);
    }

    private final String host;
    private final int port;
    private final String targetId;
    private final String password;
    private final String deviceId;
    private final Listener listener;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AtomicInteger nextMessageId = new AtomicInteger(1);
    private final ExecutorService inputExecutor = Executors.newSingleThreadExecutor(r -> {
        Thread thread = new Thread(r, "rc-input");
        thread.setDaemon(true);
        return thread;
    });
    private final Object writeLock = new Object();

    private volatile Socket socket;
    private volatile DataOutputStream output;
    private volatile long sessionId;
    private volatile boolean authenticated;
    private volatile boolean intentionalClose;
    private Thread heartbeatThread;

    RelayClient(String host, int port, String targetId, String password,
                String deviceId, Listener listener) {
        this.host = host;
        this.port = port;
        this.targetId = targetId;
        this.password = password;
        this.deviceId = deviceId;
        this.listener = listener;
    }

    void start() {
        if (!running.compareAndSet(false, true)) return;
        Thread networkThread = new Thread(this::run, "rc-relay");
        networkThread.setDaemon(true);
        networkThread.start();
        heartbeatThread = new Thread(this::heartbeatLoop, "rc-heartbeat");
        heartbeatThread.setDaemon(true);
        heartbeatThread.start();
    }

    boolean isActive() {
        return running.get();
    }

    private void run() {
        String error = null;
        try {
            listener.onStatus("正在连接中继 " + host + ":" + port);
            Socket connected = new Socket();
            socket = connected;
            connected.connect(new InetSocketAddress(host, port), 10000);
            connected.setTcpNoDelay(true);
            connected.setKeepAlive(true);
            output = new DataOutputStream(connected.getOutputStream());
            DataInputStream input = new DataInputStream(connected.getInputStream());

            sendRegister();
            while (running.get()) {
                Protocol.Frame frame = Protocol.readFrame(input);
                handleFrame(frame);
            }
        } catch (EOFException e) {
            if (!intentionalClose) error = "中继关闭了连接";
        } catch (SocketException e) {
            if (!intentionalClose) error = "网络连接中断：" + usefulMessage(e);
        } catch (Exception e) {
            if (!intentionalClose) error = usefulMessage(e);
        } finally {
            running.set(false);
            authenticated = false;
            inputExecutor.shutdownNow();
            closeSocket();
            listener.onDisconnected(intentionalClose ? "已断开连接"
                    : (error == null ? "连接已断开" : error), intentionalClose);
        }
    }

    private void handleFrame(Protocol.Frame frame) throws Exception {
        switch (frame.type) {
            case Protocol.REGISTER_ACK: {
                JSONObject response = json(frame);
                requireOk(response, "设备注册失败");
                listener.onStatus("已注册，正在请求远程设备 " + targetId);
                JSONObject want = new JSONObject();
                want.put("codec", "h264");
                want.put("max_fps", 15);
                JSONObject request = new JSONObject();
                request.put("target_id", targetId);
                request.put("mode", "control");
                request.put("want", want);
                sendJson(Protocol.CONNECT_REQ, 0, request);
                break;
            }
            case Protocol.CONNECT_RSP: {
                JSONObject response = json(frame);
                requireOk(response, "远程设备拒绝了连接");
                if (frame.sessionId == 0) throw new IOException("中继返回了无效会话");
                sessionId = frame.sessionId;
                listener.onStatus("远程设备已接受，正在鉴权");
                break;
            }
            case Protocol.AUTH_CHALLENGE: {
                JSONObject challenge = json(frame);
                byte[] nonce = Base64.decode(challenge.getString("nonce"), Base64.DEFAULT);
                JSONObject response = new JSONObject();
                response.put("password_proof", passwordProof(password, nonce));
                sendJson(Protocol.AUTH_RESPONSE, sessionId, response);
                break;
            }
            case Protocol.AUTH_RESULT: {
                JSONObject result = json(frame);
                requireOk(result, "鉴权失败");
                authenticated = true;
                listener.onStatus("鉴权成功，正在接收远程画面");
                break;
            }
            case Protocol.VIDEO_FRAME:
                if (authenticated && frame.sessionId == sessionId) {
                    listener.onVideoFrame(frame.payload);
                }
                break;
            case Protocol.HEARTBEAT:
            case Protocol.PONG:
                break;
            case Protocol.PING:
                sendJson(Protocol.PONG, frame.sessionId, new JSONObject());
                break;
            case Protocol.SESSION_CLOSE: {
                JSONObject close = json(frame);
                String reason = close.optString("reason", "远程端已结束会话");
                if (!"ack".equals(reason)) {
                    JSONObject ack = new JSONObject();
                    ack.put("reason", "ack");
                    sendJson(Protocol.SESSION_CLOSE, frame.sessionId, ack);
                }
                throw new IOException(localizeReason(reason));
            }
            case Protocol.ERROR: {
                JSONObject error = json(frame);
                throw new IOException(localizeReason(error.optString("reason", "中继返回错误")));
            }
            default:
                break;
        }
    }

    private void sendRegister() throws Exception {
        JSONObject caps = new JSONObject();
        caps.put("codec", new org.json.JSONArray().put("h264"));
        caps.put("input", true);
        JSONObject request = new JSONObject();
        request.put("device_id", deviceId);
        request.put("alias", android.os.Build.MODEL + " (Android)");
        request.put("os", "android");
        request.put("os_ver", android.os.Build.VERSION.RELEASE);
        request.put("app", "0.1.0");
        request.put("caps", caps);
        sendJson(Protocol.REGISTER, 0, request);
        listener.onStatus("已连接中继，正在注册");
    }

    private void heartbeatLoop() {
        try {
            while (running.get()) {
                Thread.sleep(10000);
                long sid = sessionId;
                if (running.get() && sid != 0) {
                    sendFrame(Protocol.HEARTBEAT, sid, new byte[0]);
                }
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (IOException e) {
            closeSocket();
        }
    }

    void close() {
        if (!running.get()) return;
        intentionalClose = true;
        long sid = sessionId;
        if (sid != 0) {
            try {
                JSONObject close = new JSONObject();
                close.put("reason", "user closed");
                sendJson(Protocol.SESSION_CLOSE, sid, close);
            } catch (Exception ignored) {
            }
        }
        running.set(false);
        authenticated = false;
        inputExecutor.shutdownNow();
        closeSocket();
        if (heartbeatThread != null) heartbeatThread.interrupt();
    }

    private void closeSocket() {
        Socket current = socket;
        socket = null;
        output = null;
        if (current != null) {
            try {
                current.close();
            } catch (IOException ignored) {
            }
        }
    }

    private void sendJson(int type, long sid, JSONObject value) throws IOException {
        sendFrame(type, sid, Protocol.json(value.toString()));
    }

    private void sendFrame(int type, long sid, byte[] payload) throws IOException {
        synchronized (writeLock) {
            DataOutputStream out = output;
            if (out == null) throw new IOException("连接尚未建立");
            out.write(Protocol.header(type, sid, nextMessageId.getAndIncrement(), payload.length));
            out.write(payload);
            out.flush();
        }
    }

    void sendMouseMove(int x, int y) {
        byte[] payload = new byte[10];
        Protocol.put32(payload, 0, timestamp());
        Protocol.put16(payload, 4, clampCoordinate(x));
        Protocol.put16(payload, 6, clampCoordinate(y));
        sendInput(Protocol.MOUSE_MOVE, payload);
    }

    void sendMouseButton(int button, boolean pressed, int x, int y) {
        byte[] payload = new byte[12];
        Protocol.put32(payload, 0, timestamp());
        payload[4] = (byte) button;
        payload[5] = (byte) (pressed ? 1 : 0);
        Protocol.put16(payload, 6, clampCoordinate(x));
        Protocol.put16(payload, 8, clampCoordinate(y));
        sendInput(Protocol.MOUSE_BUTTON, payload);
    }

    void sendMouseWheel(int deltaX, int deltaY) {
        byte[] payload = new byte[8];
        Protocol.put32(payload, 0, timestamp());
        Protocol.put16(payload, 4, deltaX);
        Protocol.put16(payload, 6, deltaY);
        sendInput(Protocol.MOUSE_WHEEL, payload);
    }

    void sendKey(long key, boolean pressed) {
        byte[] payload = new byte[12];
        Protocol.put32(payload, 0, timestamp());
        Protocol.put32(payload, 4, key);
        payload[8] = (byte) (pressed ? 1 : 0);
        sendInput(Protocol.KEY, payload);
    }

    void clickKey(long key) {
        sendKey(key, true);
        sendKey(key, false);
    }

    private void sendInput(int type, byte[] payload) {
        if (!running.get() || !authenticated || sessionId == 0) return;
        try {
            inputExecutor.execute(() -> {
                if (!running.get() || !authenticated || sessionId == 0) return;
                try {
                    sendFrame(type, sessionId, payload);
                } catch (IOException e) {
                    closeSocket();
                }
            });
        } catch (RejectedExecutionException ignored) {
        }
    }

    private static JSONObject json(Protocol.Frame frame) throws IOException {
        try {
            String text = Protocol.string(frame.payload);
            return text.isEmpty() ? new JSONObject() : new JSONObject(text);
        } catch (JSONException e) {
            throw new IOException("中继返回了无效 JSON", e);
        }
    }

    private static void requireOk(JSONObject value, String fallback) throws IOException {
        if (!value.optBoolean("ok", false)) {
            throw new IOException(localizeReason(value.optString("reason", fallback)));
        }
    }

    private static String localizeReason(String reason) {
        switch (reason) {
            case "peer disconnected": return "对方已断开连接";
            case "bad password": return "远程访问密码错误";
            case "target offline": return "目标设备不在线或未注册到中继";
            case "user closed": return "已断开连接";
            default: return reason;
        }
    }

    private static String passwordProof(String password, byte[] nonce)
            throws GeneralSecurityException {
        Mac sha = Mac.getInstance("HmacSHA256");
        byte[] key = java.security.MessageDigest.getInstance("SHA-256")
                .digest(password.getBytes(StandardCharsets.UTF_8));
        sha.init(new SecretKeySpec(key, "HmacSHA256"));
        return Base64.encodeToString(sha.doFinal(nonce), Base64.NO_WRAP);
    }

    private static long timestamp() {
        return System.currentTimeMillis() & 0xffffffffL;
    }

    private static int clampCoordinate(int value) {
        return Math.max(0, Math.min(65535, value));
    }

    private static String usefulMessage(Exception e) {
        String message = e.getMessage();
        return message == null || message.trim().isEmpty() ? e.getClass().getSimpleName() : message;
    }
}

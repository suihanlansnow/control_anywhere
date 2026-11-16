package com.rc.remote;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** rc-relay 的 24 字节小端帧协议。 */
final class Protocol {
    static final int VERSION = 1;
    static final int HEADER_SIZE = 24;
    static final int MAX_PAYLOAD = 1 << 20;

    static final int REGISTER = 0x03;
    static final int REGISTER_ACK = 0x04;
    static final int CONNECT_REQ = 0x05;
    static final int CONNECT_RSP = 0x06;
    static final int PING = 0x08;
    static final int PONG = 0x09;
    static final int HEARTBEAT = 0x0A;
    static final int AUTH_CHALLENGE = 0x13;
    static final int AUTH_RESPONSE = 0x14;
    static final int AUTH_RESULT = 0x15;
    static final int SESSION_CLOSE = 0x16;
    static final int CAPS_NEGOTIATE = 0x18;
    static final int MOUSE_MOVE = 0x20;
    static final int MOUSE_BUTTON = 0x21;
    static final int MOUSE_WHEEL = 0x22;
    static final int KEY = 0x23;
    static final int VIDEO_CONFIG = 0x30;
    static final int VIDEO_FRAME = 0x31;
    static final int ERROR = 0x61;

    private Protocol() {}

    static final class Frame {
        final int type;
        final int flags;
        final long sessionId;
        final byte[] payload;

        Frame(int type, int flags, long sessionId, byte[] payload) {
            this.type = type;
            this.flags = flags;
            this.sessionId = sessionId;
            this.payload = payload;
        }
    }

    static Frame readFrame(DataInputStream input) throws IOException {
        byte[] header = new byte[HEADER_SIZE];
        input.readFully(header);
        if (u8(header, 0) != VERSION) {
            throw new IOException("不支持的中继协议版本");
        }
        int payloadLength = i32(header, 20);
        if (payloadLength < 0 || payloadLength > MAX_PAYLOAD) {
            throw new IOException("中继数据包长度无效");
        }
        byte[] payload = new byte[payloadLength];
        input.readFully(payload);
        return new Frame(u8(header, 1), u8(header, 2), i64(header, 8), payload);
    }

    static byte[] header(int type, long sessionId, int messageId, int payloadLength) {
        byte[] out = new byte[HEADER_SIZE];
        out[0] = VERSION;
        out[1] = (byte) type;
        put32(out, 4, messageId);
        put64(out, 8, sessionId);
        put32(out, 20, payloadLength);
        return out;
    }

    static byte[] json(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    static String string(byte[] payload) throws IOException {
        if (payload.length == 0) return "";
        return new String(payload, StandardCharsets.UTF_8);
    }

    static int u8(byte[] data, int offset) {
        return data[offset] & 0xff;
    }

    static int u16(byte[] data, int offset) {
        return u8(data, offset) | (u8(data, offset + 1) << 8);
    }

    static int i32(byte[] data, int offset) {
        return u8(data, offset)
                | (u8(data, offset + 1) << 8)
                | (u8(data, offset + 2) << 16)
                | (u8(data, offset + 3) << 24);
    }

    static long i64(byte[] data, int offset) {
        long value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= ((long) u8(data, offset + i)) << (8 * i);
        }
        return value;
    }

    static void put16(byte[] out, int offset, int value) {
        out[offset] = (byte) value;
        out[offset + 1] = (byte) (value >>> 8);
    }

    static void put32(byte[] out, int offset, int value) {
        for (int i = 0; i < 4; ++i) out[offset + i] = (byte) (value >>> (8 * i));
    }

    static void put32(byte[] out, int offset, long value) {
        put32(out, offset, (int) value);
    }

    static void put64(byte[] out, int offset, long value) {
        for (int i = 0; i < 8; ++i) out[offset + i] = (byte) (value >>> (8 * i));
    }
}

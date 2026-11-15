package com.rc.remote;

import android.view.KeyEvent;

/** Android 字符/键码到 HostRunner 使用的 Qt 键值映射。 */
final class KeyMapper {
    static final long ESCAPE = 0x01000000L;
    static final long TAB = 0x01000001L;
    static final long BACKSPACE = 0x01000003L;
    static final long ENTER = 0x01000004L;
    static final long INSERT = 0x01000006L;
    static final long DELETE = 0x01000007L;
    static final long PAUSE = 0x01000008L;
    static final long PRINT = 0x01000009L;
    static final long HOME = 0x01000010L;
    static final long END = 0x01000011L;
    static final long LEFT = 0x01000012L;
    static final long UP = 0x01000013L;
    static final long RIGHT = 0x01000014L;
    static final long DOWN = 0x01000015L;
    static final long PAGE_UP = 0x01000016L;
    static final long PAGE_DOWN = 0x01000017L;
    static final long SHIFT = 0x01000020L;
    static final long CTRL = 0x01000021L;
    static final long META = 0x01000022L;
    static final long ALT = 0x01000023L;
    static final long CAPS_LOCK = 0x01000024L;
    static final long NUM_LOCK = 0x01000025L;
    static final long SCROLL_LOCK = 0x01000026L;
    static final long F1 = 0x01000030L;

    private KeyMapper() {}

    static final class Stroke {
        final long key;
        final boolean shift;

        Stroke(long key, boolean shift) {
            this.key = key;
            this.shift = shift;
        }
    }

    static Stroke forCharacter(char c) {
        if (c >= 'a' && c <= 'z') return new Stroke(c - 'a' + 'A', false);
        if (c >= 'A' && c <= 'Z') return new Stroke(c, true);
        if (c >= '0' && c <= '9') return new Stroke(c, false);
        if (c == '\n' || c == '\r') return new Stroke(ENTER, false);
        if (c == '\t') return new Stroke(TAB, false);
        switch (c) {
            case ' ': return plain(' ');
            case '!': return shifted('1');
            case '@': return shifted('2');
            case '#': return shifted('3');
            case '$': return shifted('4');
            case '%': return shifted('5');
            case '^': return shifted('6');
            case '&': return shifted('7');
            case '*': return shifted('8');
            case '(': return shifted('9');
            case ')': return shifted('0');
            case ';': return plain(';');
            case ':': return shifted(';');
            case '=': return plain('=');
            case '+': return shifted('=');
            case ',': return plain(',');
            case '<': return shifted(',');
            case '-': return plain('-');
            case '_': return shifted('-');
            case '.': return plain('.');
            case '>': return shifted('.');
            case '/': return plain('/');
            case '?': return shifted('/');
            case '`': return plain('`');
            case '~': return shifted('`');
            case '[': return plain('[');
            case '{': return shifted('[');
            case '\\': return plain('\\');
            case '|': return shifted('\\');
            case ']': return plain(']');
            case '}': return shifted(']');
            case '\'': return plain('\'');
            case '"': return shifted('\'');
            default: return null;
        }
    }

    static Stroke forAndroidKey(int keyCode, KeyEvent event) {
        if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z) {
            return new Stroke('A' + keyCode - KeyEvent.KEYCODE_A, event.isShiftPressed());
        }
        if (keyCode >= KeyEvent.KEYCODE_0 && keyCode <= KeyEvent.KEYCODE_9) {
            int unicode = event.getUnicodeChar();
            if (event.isShiftPressed() && unicode > 0 && unicode < 128) {
                Stroke shifted = forCharacter((char) unicode);
                if (shifted != null) return shifted;
            }
            return new Stroke('0' + keyCode - KeyEvent.KEYCODE_0, false);
        }
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F12) {
            return new Stroke(F1 + keyCode - KeyEvent.KEYCODE_F1, false);
        }
        switch (keyCode) {
            case KeyEvent.KEYCODE_ESCAPE: return plain(ESCAPE);
            case KeyEvent.KEYCODE_TAB: return plain(TAB);
            case KeyEvent.KEYCODE_DEL: return plain(BACKSPACE);
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return plain(ENTER);
            case KeyEvent.KEYCODE_INSERT: return plain(INSERT);
            case KeyEvent.KEYCODE_FORWARD_DEL: return plain(DELETE);
            case KeyEvent.KEYCODE_BREAK: return plain(PAUSE);
            case KeyEvent.KEYCODE_SYSRQ: return plain(PRINT);
            case KeyEvent.KEYCODE_MOVE_HOME: return plain(HOME);
            case KeyEvent.KEYCODE_MOVE_END: return plain(END);
            case KeyEvent.KEYCODE_DPAD_LEFT: return plain(LEFT);
            case KeyEvent.KEYCODE_DPAD_UP: return plain(UP);
            case KeyEvent.KEYCODE_DPAD_RIGHT: return plain(RIGHT);
            case KeyEvent.KEYCODE_DPAD_DOWN: return plain(DOWN);
            case KeyEvent.KEYCODE_PAGE_UP: return plain(PAGE_UP);
            case KeyEvent.KEYCODE_PAGE_DOWN: return plain(PAGE_DOWN);
            case KeyEvent.KEYCODE_SHIFT_LEFT:
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return plain(SHIFT);
            case KeyEvent.KEYCODE_CTRL_LEFT:
            case KeyEvent.KEYCODE_CTRL_RIGHT: return plain(CTRL);
            case KeyEvent.KEYCODE_META_LEFT:
            case KeyEvent.KEYCODE_META_RIGHT: return plain(META);
            case KeyEvent.KEYCODE_ALT_LEFT:
            case KeyEvent.KEYCODE_ALT_RIGHT: return plain(ALT);
            case KeyEvent.KEYCODE_CAPS_LOCK: return plain(CAPS_LOCK);
            case KeyEvent.KEYCODE_NUM_LOCK: return plain(NUM_LOCK);
            case KeyEvent.KEYCODE_SCROLL_LOCK: return plain(SCROLL_LOCK);
            case KeyEvent.KEYCODE_SPACE: return plain(' ');
            default: break;
        }
        int unicode = event.getUnicodeChar();
        if (unicode > 0 && unicode < 128 && !Character.isISOControl(unicode)) {
            return forCharacter((char) unicode);
        }
        return null;
    }

    static boolean isModifier(long key) {
        return key == SHIFT || key == CTRL || key == ALT || key == META;
    }

    private static Stroke plain(long key) {
        return new Stroke(key, false);
    }

    private static Stroke shifted(long key) {
        return new Stroke(key, true);
    }
}

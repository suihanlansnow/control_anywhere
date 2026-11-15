package com.rc.remote;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.UUID;

public final class MainActivity extends Activity {
    private static final String PREFS = "connection";
    private static final int DARK = Color.rgb(32, 33, 36);
    private static final int CONTROL = Color.rgb(60, 64, 67);
    private static final int ACCENT = Color.rgb(25, 118, 210);

    private FrameLayout root;
    private View loginView;
    private View remoteView;
    private EditText relayInput;
    private EditText targetInput;
    private EditText passwordInput;
    private TextView loginStatus;
    private TextView remoteStatus;
    private Button connectButton;
    private Button rightButton;
    private Button ctrlButton;
    private Button altButton;
    private Button metaButton;
    private RemoteSurfaceView surface;
    private SharedPreferences preferences;
    private RelayClient client;
    private boolean showingRemote;
    private boolean ctrlDown;
    private boolean altDown;
    private boolean metaDown;
    private int connectionGeneration;
    private String terminalError;

    private final H264Decoder.Listener decoderListener = new H264Decoder.Listener() {
        @Override
        public void onFirstFrame() {
            runOnUiThread(() -> {
                if (showingRemote) remoteStatus.setText("已连接，可远程操控");
            });
        }

        @Override
        public void onDecoderError(String message) {
            runOnUiThread(() -> {
                if (!showingRemote || client == null) return;
                terminalError = message;
                client.close();
            });
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences(PREFS, MODE_PRIVATE);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        setContentView(root);
        buildLoginView();
        buildRemoteView();
        showLogin("请输入中继和被控端信息");
    }

    private void buildLoginView() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(245, 246, 248));
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setGravity(Gravity.CENTER_HORIZONTAL);
        int padding = dp(24);
        content.setPadding(padding, padding, padding, padding);
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText("RC 远程桌面");
        title.setTextSize(28);
        title.setTextColor(DARK);
        title.setGravity(Gravity.CENTER);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        content.addView(title, margins(ViewGroup.LayoutParams.MATCH_PARENT, 0, 0, 0, 8));

        TextView subtitle = new TextView(this);
        subtitle.setText("连接 Windows / Ubuntu 被控端");
        subtitle.setTextSize(16);
        subtitle.setTextColor(Color.DKGRAY);
        subtitle.setGravity(Gravity.CENTER);
        content.addView(subtitle, margins(ViewGroup.LayoutParams.MATCH_PARENT, 0, 0, 0, 24));

        relayInput = addField(content, "中继地址", "中继 IP 或域名:21116",
                preferences.getString("relay", "127.0.0.1:21116"), false);
        targetInput = addField(content, "被控设备 ID", "HostRunner 注册的设备 ID",
                preferences.getString("target", ""), false);
        passwordInput = addField(content, "远程访问密码", "不会被保存", "", true);

        connectButton = new Button(this);
        connectButton.setText("连接远程桌面");
        connectButton.setTextSize(17);
        connectButton.setOnClickListener(v -> connect());
        content.addView(connectButton, margins(ViewGroup.LayoutParams.MATCH_PARENT, 0, 20, 0, 12));

        loginStatus = new TextView(this);
        loginStatus.setTextSize(14);
        loginStatus.setTextColor(Color.DKGRAY);
        loginStatus.setGravity(Gravity.CENTER);
        loginStatus.setPadding(0, dp(8), 0, dp(8));
        content.addView(loginStatus, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView help = new TextView(this);
        help.setText("触控：单指点击/拖动，长按右键，双指上下滚动。\n"
                + "底部提供屏幕键盘、特殊键及 Ctrl / Alt / Win 组合键。\n"
                + "注意：中继地址必须是手机可访问的地址，不能填被控端的 127.0.0.1。");
        help.setTextSize(13);
        help.setTextColor(Color.GRAY);
        help.setLineSpacing(0, 1.25f);
        content.addView(help, margins(ViewGroup.LayoutParams.MATCH_PARENT, 0, 20, 0, 0));

        loginView = scroll;
        root.addView(scroll, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private EditText addField(LinearLayout parent, String label, String hint,
                              String value, boolean password) {
        TextView labelView = new TextView(this);
        labelView.setText(label);
        labelView.setTextSize(14);
        labelView.setTextColor(DARK);
        parent.addView(labelView, margins(ViewGroup.LayoutParams.MATCH_PARENT, 0, 10, 0, 2));

        EditText input = new EditText(this);
        input.setHint(hint);
        input.setText(value);
        input.setSingleLine(true);
        input.setTextSize(16);
        input.setPadding(dp(12), 0, dp(12), 0);
        if (password) {
            input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        } else {
            input.setInputType(InputType.TYPE_CLASS_TEXT);
        }
        parent.addView(input, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(52)));
        return input;
    }

    private void buildRemoteView() {
        LinearLayout remote = new LinearLayout(this);
        remote.setOrientation(LinearLayout.VERTICAL);
        remote.setBackgroundColor(Color.BLACK);

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(12), dp(4), dp(6), dp(4));
        top.setBackgroundColor(DARK);
        remote.addView(top, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        remoteStatus = new TextView(this);
        remoteStatus.setTextColor(Color.WHITE);
        remoteStatus.setTextSize(14);
        remoteStatus.setGravity(Gravity.CENTER_VERTICAL);
        remoteStatus.setMaxLines(2);
        top.addView(remoteStatus, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1));

        Button disconnect = controlButton("断开");
        disconnect.setOnClickListener(v -> disconnect());
        top.addView(disconnect, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(44)));

        surface = new RemoteSurfaceView(this, decoderListener);
        surface.setListener(enabled -> updateToggle(rightButton, enabled));
        FrameLayout videoContainer = new FrameLayout(this);
        videoContainer.setBackgroundColor(Color.BLACK);
        videoContainer.addView(surface, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));
        remote.addView(videoContainer, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        HorizontalScrollView controls = new HorizontalScrollView(this);
        controls.setHorizontalScrollBarEnabled(false);
        controls.setBackgroundColor(DARK);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(6), dp(4), dp(6), dp(4));
        controls.addView(row, new HorizontalScrollView.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        Button keyboard = controlButton("键盘");
        keyboard.setOnClickListener(v -> showKeyboard());
        row.addView(keyboard, controlMargins());
        Button special = controlButton("特殊键");
        special.setOnClickListener(v -> showSpecialKeys());
        row.addView(special, controlMargins());
        rightButton = controlButton("右键");
        rightButton.setOnClickListener(v -> surface.setRightButtonMode(!surface.isRightButtonMode()));
        row.addView(rightButton, controlMargins());
        ctrlButton = controlButton("Ctrl");
        ctrlButton.setOnClickListener(v -> toggleModifier(KeyMapper.CTRL));
        row.addView(ctrlButton, controlMargins());
        altButton = controlButton("Alt");
        altButton.setOnClickListener(v -> toggleModifier(KeyMapper.ALT));
        row.addView(altButton, controlMargins());
        metaButton = controlButton("Win");
        metaButton.setOnClickListener(v -> toggleModifier(KeyMapper.META));
        row.addView(metaButton, controlMargins());
        Button wheelUp = controlButton("滚轮 ↑");
        wheelUp.setOnClickListener(v -> {
            if (client != null) client.sendMouseWheel(0, 120);
        });
        row.addView(wheelUp, controlMargins());
        Button wheelDown = controlButton("滚轮 ↓");
        wheelDown.setOnClickListener(v -> {
            if (client != null) client.sendMouseWheel(0, -120);
        });
        row.addView(wheelDown, controlMargins());
        remote.addView(controls, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        remoteView = remote;
        remoteView.setVisibility(View.GONE);
        root.addView(remote, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private Button controlButton(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextColor(Color.WHITE);
        button.setTextSize(13);
        button.setBackgroundColor(CONTROL);
        button.setMinHeight(dp(42));
        button.setMinimumHeight(dp(42));
        button.setPadding(dp(12), 0, dp(12), 0);
        return button;
    }

    private LinearLayout.LayoutParams controlMargins() {
        return margins(ViewGroup.LayoutParams.WRAP_CONTENT, 3, 2, 3, 2);
    }

    private void connect() {
        final String relay = relayInput.getText().toString().trim();
        final String target = targetInput.getText().toString().trim();
        final String password = passwordInput.getText().toString();
        final HostPort address;
        try {
            address = HostPort.parse(relay);
        } catch (IllegalArgumentException e) {
            loginStatus.setText(e.getMessage());
            return;
        }
        if (target.isEmpty()) {
            loginStatus.setText("请输入被控设备 ID");
            targetInput.requestFocus();
            return;
        }

        preferences.edit().putString("relay", relay).putString("target", target).apply();
        hideKeyboard();
        final int generation = ++connectionGeneration;
        terminalError = null;
        resetModifiers(false);
        surface.setRightButtonMode(false);
        remoteStatus.setText("正在连接…");
        loginView.setVisibility(View.GONE);
        remoteView.setVisibility(View.VISIBLE);
        showingRemote = true;

        client = new RelayClient(address.host, address.port, target, password, deviceId(),
                new RelayClient.Listener() {
                    @Override
                    public void onStatus(String status) {
                        runOnUiThread(() -> {
                            if (generation == connectionGeneration && showingRemote) {
                                remoteStatus.setText(status);
                            }
                        });
                    }

                    @Override
                    public void onVideoFrame(byte[] payload) {
                        if (generation == connectionGeneration) surface.offerFrame(payload);
                    }

                    @Override
                    public void onDisconnected(String reason, boolean intentional) {
                        runOnUiThread(() -> {
                            if (generation != connectionGeneration) return;
                            surface.setClient(null);
                            resetModifiers(false);
                            String message = terminalError != null ? terminalError : reason;
                            showLogin(intentional && terminalError == null ? "已断开连接"
                                    : "连接结束：" + message);
                            terminalError = null;
                        });
                    }
                });
        surface.setClient(client);
        client.start();
    }

    private void disconnect() {
        if (client != null) {
            remoteStatus.setText("正在断开…");
            client.close();
        } else {
            showLogin("已断开连接");
        }
    }

    private void showLogin(String status) {
        showingRemote = false;
        remoteView.setVisibility(View.GONE);
        loginView.setVisibility(View.VISIBLE);
        connectButton.setEnabled(true);
        loginStatus.setText(status);
        loginStatus.setTextColor(status.startsWith("连接结束") || status.startsWith("H.264")
                ? Color.rgb(180, 40, 40) : Color.DKGRAY);
    }

    private void showKeyboard() {
        EditText input = new EditText(this);
        input.setHint("英文、数字和常用 ASCII 符号");
        input.setMinLines(3);
        input.setGravity(Gravity.TOP | Gravity.START);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        int padding = dp(20);
        input.setPadding(padding, dp(8), padding, dp(8));
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("发送到远程键盘")
                .setView(input)
                .setPositiveButton("发送", (d, which) -> sendText(input.getText().toString()))
                .setNegativeButton("取消", null)
                .create();
        dialog.setOnShowListener(d -> {
            input.requestFocus();
            dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        });
        dialog.show();
    }

    private void sendText(String text) {
        if (client == null) return;
        int skipped = 0;
        for (int i = 0; i < text.length(); ++i) {
            KeyMapper.Stroke stroke = KeyMapper.forCharacter(text.charAt(i));
            if (stroke == null) {
                ++skipped;
                continue;
            }
            sendStroke(stroke);
        }
        if (skipped > 0) {
            Toast.makeText(this, "已忽略 " + skipped + " 个非 ASCII 字符；请使用被控端支持的键盘输入",
                    Toast.LENGTH_LONG).show();
        }
    }

    private void sendStroke(KeyMapper.Stroke stroke) {
        if (client == null) return;
        if (stroke.shift) client.sendKey(KeyMapper.SHIFT, true);
        client.sendKey(stroke.key, true);
        client.sendKey(stroke.key, false);
        if (stroke.shift) client.sendKey(KeyMapper.SHIFT, false);
    }

    private void showSpecialKeys() {
        final String[] labels = {"Esc", "Tab", "Enter", "Backspace", "Delete", "Insert",
                "←", "↑", "↓", "→", "Home", "End", "Page Up", "Page Down",
                "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
                "Caps Lock", "Print Screen", "Pause"};
        final long[] keys = {KeyMapper.ESCAPE, KeyMapper.TAB, KeyMapper.ENTER, KeyMapper.BACKSPACE,
                KeyMapper.DELETE, KeyMapper.INSERT, KeyMapper.LEFT, KeyMapper.UP, KeyMapper.DOWN,
                KeyMapper.RIGHT, KeyMapper.HOME, KeyMapper.END, KeyMapper.PAGE_UP, KeyMapper.PAGE_DOWN,
                KeyMapper.F1, KeyMapper.F1 + 1, KeyMapper.F1 + 2, KeyMapper.F1 + 3,
                KeyMapper.F1 + 4, KeyMapper.F1 + 5, KeyMapper.F1 + 6, KeyMapper.F1 + 7,
                KeyMapper.F1 + 8, KeyMapper.F1 + 9, KeyMapper.F1 + 10, KeyMapper.F1 + 11,
                KeyMapper.CAPS_LOCK, KeyMapper.PRINT, KeyMapper.PAUSE};
        new AlertDialog.Builder(this)
                .setTitle("特殊键（可配合 Ctrl / Alt / Win）")
                .setItems(labels, (dialog, which) -> {
                    if (client != null) client.clickKey(keys[which]);
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void toggleModifier(long key) {
        if (client == null) return;
        boolean pressed;
        if (key == KeyMapper.CTRL) {
            ctrlDown = !ctrlDown;
            pressed = ctrlDown;
        } else if (key == KeyMapper.ALT) {
            altDown = !altDown;
            pressed = altDown;
        } else {
            metaDown = !metaDown;
            pressed = metaDown;
        }
        client.sendKey(key, pressed);
        updateModifierButtons();
    }

    private void resetModifiers(boolean send) {
        if (send && client != null) {
            if (ctrlDown) client.sendKey(KeyMapper.CTRL, false);
            if (altDown) client.sendKey(KeyMapper.ALT, false);
            if (metaDown) client.sendKey(KeyMapper.META, false);
        }
        ctrlDown = altDown = metaDown = false;
        updateModifierButtons();
    }

    private void updateModifierButtons() {
        updateToggle(ctrlButton, ctrlDown);
        updateToggle(altButton, altDown);
        updateToggle(metaButton, metaDown);
    }

    private static void updateToggle(Button button, boolean selected) {
        if (button != null) button.setBackgroundColor(selected ? ACCENT : CONTROL);
    }

    private void hideKeyboard() {
        View focused = getCurrentFocus();
        if (focused != null) {
            InputMethodManager manager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            manager.hideSoftInputFromWindow(focused.getWindowToken(), 0);
            focused.clearFocus();
        }
    }

    private String deviceId() {
        String id = preferences.getString("device_id", "");
        if (id.isEmpty()) {
            id = "android-" + UUID.randomUUID().toString().replace("-", "").substring(0, 12);
            preferences.edit().putString("device_id", id).apply();
        }
        return id;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (showingRemote && client != null && event.getKeyCode() != KeyEvent.KEYCODE_BACK) {
            KeyMapper.Stroke stroke = KeyMapper.forAndroidKey(event.getKeyCode(), event);
            if (stroke != null) {
                if (event.getAction() == KeyEvent.ACTION_DOWN) {
                    if (event.getRepeatCount() == 0 || !KeyMapper.isModifier(stroke.key)) {
                        client.sendKey(stroke.key, true);
                    }
                    return true;
                }
                if (event.getAction() == KeyEvent.ACTION_UP) {
                    client.sendKey(stroke.key, false);
                    return true;
                }
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onBackPressed() {
        if (!showingRemote) {
            super.onBackPressed();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("断开远程桌面？")
                .setPositiveButton("断开", (dialog, which) -> disconnect())
                .setNegativeButton("继续控制", null)
                .show();
    }

    @Override
    protected void onDestroy() {
        ++connectionGeneration;
        if (client != null) client.close();
        surface.setClient(null);
        surface.release();
        super.onDestroy();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private LinearLayout.LayoutParams margins(int width, int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(width,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        params.setMargins(dp(left), dp(top), dp(right), dp(bottom));
        return params;
    }

    private static final class HostPort {
        final String host;
        final int port;

        HostPort(String host, int port) {
            this.host = host;
            this.port = port;
        }

        static HostPort parse(String text) {
            if (text.isEmpty()) throw new IllegalArgumentException("请输入中继地址");
            String host = text;
            int port = 21116;
            if (text.startsWith("[")) {
                int end = text.indexOf(']');
                if (end < 0) throw new IllegalArgumentException("IPv6 地址格式无效");
                host = text.substring(1, end);
                String suffix = text.substring(end + 1);
                if (!suffix.isEmpty()) {
                    if (!suffix.startsWith(":")) throw new IllegalArgumentException("中继地址格式无效");
                    port = parsePort(suffix.substring(1));
                }
            } else {
                int colon = text.lastIndexOf(':');
                if (colon >= 0 && text.indexOf(':') == colon) {
                    host = text.substring(0, colon);
                    port = parsePort(text.substring(colon + 1));
                }
            }
            if (host.trim().isEmpty()) throw new IllegalArgumentException("中继主机不能为空");
            return new HostPort(host.trim(), port);
        }

        private static int parsePort(String text) {
            try {
                int port = Integer.parseInt(text);
                if (port < 1 || port > 65535) throw new NumberFormatException();
                return port;
            } catch (NumberFormatException e) {
                throw new IllegalArgumentException("中继端口必须是 1–65535");
            }
        }
    }
}

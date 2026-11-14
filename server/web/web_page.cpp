#include "web_page.h"

namespace rc::web {

const char* web_page_html() {
  return R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>远程控制 · Web</title>
<style>
  :root {
    color-scheme: dark;
    --bg:#0e1014; --panel:#171a21; --line:#262b36;
    --text:#e6e9ef; --muted:#8d95a5; --accent:#2fbf71;
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; margin: 0; }
  body {
    background: var(--bg); color: var(--text); overflow: hidden;
    font: 14px/1.5 "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
    display: flex; flex-direction: column;
  }
  header {
    display: flex; align-items: center; gap: 14px; flex-wrap: wrap;
    padding: 10px 16px; background: var(--panel); border-bottom: 1px solid var(--line);
  }
  .brand { font-weight: 600; letter-spacing: .5px; }
  .brand span { color: var(--accent); }
  .pill {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 3px 10px; border-radius: 999px; background: #1e222b;
    color: var(--muted); font-size: 12px;
  }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: #e5534b; }
  .dot.on { background: var(--accent); box-shadow: 0 0 8px rgba(47,191,113,.8); }
  .meta { color: var(--muted); font-size: 12px; font-variant-numeric: tabular-nums; }
  .spacer { flex: 1; }
  button {
    background: #232833; color: var(--text); border: 1px solid var(--line);
    border-radius: 6px; padding: 5px 12px; font-size: 13px; cursor: pointer;
  }
  button:hover { border-color: #3a4152; }
  button[aria-pressed="false"] { color: var(--muted); }
  main { flex: 1; min-height: 0; padding: 12px; }
  #stage {
    position: relative; width: 100%; height: 100%;
    background: #000; border-radius: 8px; overflow: hidden; cursor: crosshair;
  }
  #stage:fullscreen { border-radius: 0; }
  #screen {
    display: block; width: 100%; height: 100%; object-fit: contain;
    user-select: none; -webkit-user-drag: none;
  }
  #overlay {
    position: absolute; inset: 0; display: flex; flex-direction: column;
    align-items: center; justify-content: center; gap: 6px; padding: 24px;
    background: rgba(8,10,14,.72); color: var(--muted); text-align: center;
  }
  #overlay.hidden { display: none; }
  #overlay strong { color: var(--text); font-size: 15px; font-weight: 500; }
  footer {
    display: flex; gap: 18px; flex-wrap: wrap;
    padding: 8px 16px 12px; color: var(--muted); font-size: 12px;
  }
  kbd {
    background: #232833; border: 1px solid var(--line); border-bottom-width: 2px;
    border-radius: 4px; padding: 0 5px; color: var(--text); font-size: 11px;
  }
</style>
</head>
<body>
<header>
  <div class="brand">远程控制 <span>Web</span></div>
  <div class="pill"><span id="dot" class="dot"></span><span id="stateText">连接中…</span></div>
  <div class="meta" id="meta">--</div>
  <div class="spacer"></div>
  <button id="kbBtn" aria-pressed="true">键盘：开</button>
  <button id="fsBtn">全屏</button>
</header>
<main>
  <div id="stage">
    <img id="screen" alt="远程画面" />
    <div id="overlay">
      <strong>等待远程画面</strong>
      <span id="overlayText">正在连接被控端…</span>
    </div>
  </div>
</main>
<footer>
  <span>左键 / 右键 / 滚轮直接作用于远程桌面</span>
  <span>键盘需保持本页面为前台窗口</span>
  <span><kbd>Ctrl</kbd>+<kbd>W</kbd> 等浏览器级快捷键无法转发</span>
</footer>
<script>
(function () {
  const img = document.getElementById('screen');
  const stage = document.getElementById('stage');
  const overlay = document.getElementById('overlay');
  const overlayText = document.getElementById('overlayText');
  const dot = document.getElementById('dot');
  const stateText = document.getElementById('stateText');
  const meta = document.getElementById('meta');
  const kbBtn = document.getElementById('kbBtn');
  const fsBtn = document.getElementById('fsBtn');

  const STATE_TEXT = {
    connecting: '连接中…', connected: '已连接中继', registered: '已注册',
    authing: '鉴权中…', streaming: '画面传输中', retrying: '等待被控端…',
    auth_failed: '密码错误', closed: '会话已关闭', offline: '被控端离线'
  };

  let kbEnabled = true;
  let lastPoint = { x: 0, y: 0 };
  let lastMoveSent = 0;
  let pendingMove = null;
  let moveTimer = 0;
  let wheelDx = 0;
  let wheelDy = 0;
  let wheelTimer = 0;
  const pressedButtons = new Set();
  const pressedKeys = new Set();

  function post(payload) {
    fetch('/input', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      keepalive: true
    }).catch(function () {});
  }

  // 画面按 object-fit: contain 显示，这里还原出帧内像素坐标并排除黑边。
  function remotePoint(clientX, clientY) {
    const nw = img.naturalWidth;
    const nh = img.naturalHeight;
    if (!nw || !nh) return null;
    const rect = stage.getBoundingClientRect();
    const scale = Math.min(rect.width / nw, rect.height / nh);
    if (!scale) return null;
    const left = rect.left + (rect.width - nw * scale) / 2;
    const top = rect.top + (rect.height - nh * scale) / 2;
    const x = Math.floor((clientX - left) / scale);
    const y = Math.floor((clientY - top) / scale);
    if (x < 0 || y < 0 || x >= nw || y >= nh) return null;
    return { x: x, y: y };
  }

  function sendMove(point) {
    lastPoint = point;
    lastMoveSent = Date.now();
    pendingMove = null;
    post({ type: 'move', x: point.x, y: point.y });
  }

  function queueMove(point) {
    pendingMove = point;
    if (moveTimer) return;
    const wait = Math.max(0, 25 - (Date.now() - lastMoveSent));
    moveTimer = setTimeout(function () {
      moveTimer = 0;
      if (pendingMove) sendMove(pendingMove);
    }, wait);
  }

  stage.addEventListener('pointermove', function (event) {
    const point = remotePoint(event.clientX, event.clientY);
    if (point) queueMove(point);
  });

  stage.addEventListener('pointerdown', function (event) {
    const point = remotePoint(event.clientX, event.clientY);
    if (!point) return;
    if (stage.setPointerCapture) {
      try { stage.setPointerCapture(event.pointerId); } catch (e) {}
    }
    const button = event.button === 0 ? 1 : event.button === 1 ? 3
                 : event.button === 2 ? 2 : 0;
    if (!button) return;
    pressedButtons.add(button);
    sendMove(point);
    post({ type: 'button', button: button, state: 1, x: point.x, y: point.y });
    event.preventDefault();
  });

  stage.addEventListener('pointerup', function (event) {
    const button = event.button === 0 ? 1 : event.button === 1 ? 3
                 : event.button === 2 ? 2 : 0;
    if (!button) return;
    pressedButtons.delete(button);
    post({ type: 'button', button: button, state: 0,
           x: lastPoint.x, y: lastPoint.y });
    event.preventDefault();
  });

  stage.addEventListener('contextmenu', function (event) { event.preventDefault(); });

  // 触控板一次滑动会产生大量滚轮事件，按 16ms 合并成一个增量，
  // 否则每个事件都占一条输入消息，被控端会排队造成"卡住"的错觉。
  stage.addEventListener('wheel', function (event) {
    event.preventDefault();
    wheelDx += event.deltaX ? Math.sign(event.deltaX) * 120 : 0;
    wheelDy += event.deltaY ? Math.sign(event.deltaY) * 120 : 0;
    if (wheelTimer) return;
    wheelTimer = setTimeout(function () {
      wheelTimer = 0;
      const dx = Math.max(-32767, Math.min(32767, wheelDx));
      const dy = Math.max(-32767, Math.min(32767, wheelDy));
      wheelDx = 0;
      wheelDy = 0;
      if (dx || dy) post({ type: 'wheel', dx: dx, dy: dy });
    }, 16);
  }, { passive: false });

  // 浏览器按键 -> Qt::Key 整数，和被控端注入器的映射表保持一致。
  const SPECIAL = {
    Escape: 0x01000000, Tab: 0x01000001, Backspace: 0x01000003,
    Enter: 0x01000004, Insert: 0x01000006, Delete: 0x01000007,
    Pause: 0x01000008, PrintScreen: 0x01000009,
    Home: 0x01000010, End: 0x01000011,
    ArrowLeft: 0x01000012, ArrowUp: 0x01000013,
    ArrowRight: 0x01000014, ArrowDown: 0x01000015,
    PageUp: 0x01000016, PageDown: 0x01000017,
    CapsLock: 0x01000024, NumLock: 0x01000025, ScrollLock: 0x01000026
  };

  function qtKey(event) {
    const code = event.code || '';
    if (code.indexOf('Shift') === 0) return 0x01000020;
    if (code.indexOf('Control') === 0) return 0x01000021;
    if (code.indexOf('Alt') === 0) return 0x01000023;
    if (code.indexOf('Meta') === 0) return 0x01000022;
    if (/^Key[A-Z]$/.test(code)) return code.charCodeAt(3);
    if (/^Digit[0-9]$/.test(code)) return code.charCodeAt(5);
    if (/^Numpad[0-9]$/.test(code)) return code.charCodeAt(6);
    if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) {
      return 0x01000030 + Number(code.slice(1)) - 1;
    }
    if (SPECIAL[code] !== undefined) return SPECIAL[code];
    if (event.key === ' ') return 0x20;
    if (event.key && event.key.length === 1) {
      const ch = event.key.charCodeAt(0);
      if (ch > 0x20 && ch < 0x7f) return ch;
    }
    return 0;
  }

  function releaseAll() {
    pressedKeys.forEach(function (key) {
      post({ type: 'key', key: key, state: 0 });
    });
    pressedKeys.clear();
    pressedButtons.forEach(function (button) {
      post({ type: 'button', button: button, state: 0,
             x: lastPoint.x, y: lastPoint.y });
    });
    pressedButtons.clear();
  }

  window.addEventListener('keydown', function (event) {
    if (!kbEnabled) return;
    const key = qtKey(event);
    if (!key) return;
    event.preventDefault();
    if (event.repeat) {
      post({ type: 'key', key: key, state: 2 });
      return;
    }
    if (pressedKeys.has(key)) return;
    pressedKeys.add(key);
    post({ type: 'key', key: key, state: 1 });
  });

  window.addEventListener('keyup', function (event) {
    if (!kbEnabled) return;
    const key = qtKey(event);
    if (!key) return;
    event.preventDefault();
    pressedKeys.delete(key);
    post({ type: 'key', key: key, state: 0 });
  });

  window.addEventListener('blur', releaseAll);
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) releaseAll();
  });

  kbBtn.addEventListener('click', function () {
    kbEnabled = !kbEnabled;
    kbBtn.setAttribute('aria-pressed', String(kbEnabled));
    kbBtn.textContent = kbEnabled ? '键盘：开' : '键盘：关';
    if (!kbEnabled) releaseAll();
  });

  fsBtn.addEventListener('click', function () {
    if (!document.fullscreenElement) {
      if (stage.requestFullscreen) stage.requestFullscreen();
    } else if (document.exitFullscreen) {
      document.exitFullscreen();
    }
  });
  document.addEventListener('fullscreenchange', function () {
    fsBtn.textContent = document.fullscreenElement ? '退出全屏' : '全屏';
  });

  let streamTimer = 0;
  function startStream() {
    clearTimeout(streamTimer);
    img.src = '/stream?t=' + Date.now();
  }
  img.addEventListener('error', function () {
    overlay.classList.remove('hidden');
    overlayText.textContent = '画面中断，正在重连…';
    streamTimer = setTimeout(startStream, 1000);
  });
  startStream();

  async function poll() {
    let alive = false;
    try {
      const response = await fetch('/status', { cache: 'no-store' });
      const status = await response.json();
      alive = true;
      const streaming = !!status.streaming && status.age_ms < 4000;
      dot.classList.toggle('on', streaming);
      stateText.textContent = STATE_TEXT[status.state] || status.state || '未知';
      if (status.width > 0) {
        meta.textContent = status.width + '×' + status.height + ' · '
          + status.fps + ' fps · ' + status.viewers + ' 观看';
      } else {
        meta.textContent = '--';
      }
      if (streaming) {
        overlay.classList.add('hidden');
      } else {
        overlay.classList.remove('hidden');
        overlayText.textContent = status.state === 'auth_failed'
          ? '密码校验失败，请检查 rc-web 启动参数 --password'
          : '正在等待被控端画面…';
      }
    } catch (e) {
      alive = false;
    }
    if (!alive) {
      dot.classList.remove('on');
      stateText.textContent = '网关未响应';
    }
    setTimeout(poll, alive ? 1000 : 2000);
  }
  poll();
})();
</script>
</body>
</html>
)HTML";
}

}  // namespace rc::web

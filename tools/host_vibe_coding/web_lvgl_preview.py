"""Launcher for the real ESP-Claw Lua LVGL WebAssembly simulator."""

from __future__ import annotations

import html
import json
import os
import socket
import tempfile
import threading
import time
import urllib.parse
import webbrowser
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import lua_module_catalog as catalog


REPO_ROOT = catalog.REPO_ROOT
SIM_SOURCE_DIR = REPO_ROOT / "tools" / "lua_lvgl_web_sim"
DEFAULT_BUILD_DIRS = (
    REPO_ROOT / "build" / "lua_lvgl_web_sim",
    SIM_SOURCE_DIR / "build",
)


@dataclass(frozen=True)
class WebLvglSimArtifacts:
    root: Path
    html_name: str
    single_file: bool = False


@dataclass(frozen=True)
class WebLvglCheckResult:
    ok: bool
    skipped: bool = False
    reason: str = ""
    log: str = ""
    url: str = ""
    exit_code: int | None = None
    runtime_s: float = 0.0
    screenshot_path: str = ""
    visual_summary: str = ""

    def describe(self) -> str:
        state = "skipped" if self.skipped else ("ok" if self.ok else "failed")
        parts = [f"web_preview={state}"]
        if self.runtime_s:
            parts.append(f"runtime_s={self.runtime_s:.1f}")
        if self.exit_code is not None:
            parts.append(f"exit_code={self.exit_code}")
        if self.reason:
            parts.append(f"reason={self.reason}")
        if self.visual_summary:
            parts.append(self.visual_summary)
        if self.screenshot_path:
            parts.append(f"screenshot={self.screenshot_path}")
        text = " ".join(parts)
        if self.log:
            text += "\nWeb 预览日志：\n" + self.log[-1800:]
        return text


def find_web_lvgl_sim() -> WebLvglSimArtifacts | None:
    """Find a built lua_lvgl_web_sim output directory."""
    candidates: list[Path] = []
    env_dir = os.environ.get("ESP_CLAW_LVGL_SIM_DIR", "").strip()
    if env_dir:
        candidates.append(Path(env_dir).expanduser())
    candidates.extend(DEFAULT_BUILD_DIRS)

    for root in candidates:
        if not root.exists():
            continue
        release = root / "esp_claw_sim_release.html"
        if release.exists():
            return WebLvglSimArtifacts(root=root, html_name=release.name, single_file=True)
        normal = root / "esp_claw_sim.html"
        if normal.exists() and (root / "esp_claw_sim.js").exists():
            return WebLvglSimArtifacts(root=root, html_name=normal.name, single_file=False)
    return None


def build_instructions() -> str:
    return (
        "未找到 lua_lvgl_web_sim 构建产物。请先在 Emscripten 环境中构建：\n"
        "emcmake cmake -S tools/lua_lvgl_web_sim -B build/lua_lvgl_web_sim -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
        "embuilder build sdl2\n"
        "emmake ninja -C build/lua_lvgl_web_sim esp_claw_sim esp_claw_sim_release\n\n"
        "也可以设置 ESP_CLAW_LVGL_SIM_DIR 指向包含 esp_claw_sim.html 的目录。"
    )


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class WebLvglPreviewServer:
    def __init__(self, artifacts: WebLvglSimArtifacts) -> None:
        self.artifacts = artifacts
        self._script = ""
        self._name = "preview.lua"
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self.url = ""

    def start(self) -> None:
        if self._server:
            return
        port = _free_port()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format: str, *args: Any) -> None:
                return

            def do_GET(self) -> None:  # noqa: N802
                parsed = urllib.parse.urlparse(self.path)
                if parsed.path == "/" or parsed.path == "/preview":
                    self._send_text(owner._preview_html(), "text/html; charset=utf-8")
                    return
                if parsed.path == "/script.lua":
                    self._send_text(owner._script, "text/x-lua; charset=utf-8")
                    return
                if parsed.path.startswith("/sim/"):
                    rel = parsed.path.removeprefix("/sim/")
                    owner._send_file(self, rel)
                    return
                self.send_error(404)

            def _send_text(self, text: str, content_type: str) -> None:
                data = text.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)

        self._server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.url = f"http://127.0.0.1:{port}/preview"
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        if not self._server:
            return
        self._server.shutdown()
        self._server.server_close()
        self._server = None

    def open_script(self, script: str, name: str = "preview.lua") -> str:
        self.set_script(script, name)
        webbrowser.open(self.url)
        return self.url

    def set_script(self, script: str, name: str = "preview.lua") -> str:
        self._script = script
        self._name = name
        self.start()
        return self.url

    def _send_file(self, handler: BaseHTTPRequestHandler, rel: str) -> None:
        path = (self.artifacts.root / rel).resolve()
        root = self.artifacts.root.resolve()
        if root not in path.parents and path != root:
            handler.send_error(403)
            return
        if not path.exists() or not path.is_file():
            handler.send_error(404)
            return
        content_type = "application/octet-stream"
        if path.suffix == ".html":
            content_type = "text/html; charset=utf-8"
        elif path.suffix == ".js":
            content_type = "application/javascript; charset=utf-8"
        elif path.suffix == ".wasm":
            content_type = "application/wasm"
        elif path.suffix == ".data":
            content_type = "application/octet-stream"
        data = path.read_bytes()
        handler.send_response(200)
        handler.send_header("Content-Type", content_type)
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Cross-Origin-Opener-Policy", "same-origin")
        handler.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        handler.end_headers()
        handler.wfile.write(data)

    def _preview_html(self) -> str:
        sim_src = "/sim/" + urllib.parse.quote(self.artifacts.html_name) + "?embedded=1"
        name = html.escape(self._name or f"preview_{int(time.time())}.lua")
        script_json = json.dumps(self._script)
        script_path_json = json.dumps(f"/uploads/scripts/{self._name or 'preview.lua'}")
        return f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP-Claw LVGL Lua Preview</title>
  <style>
    html, body {{ margin: 0; height: 100%; background: #0b1020; color: #dbeafe; font-family: system-ui, sans-serif; }}
    #bar {{ height: 38px; display: flex; align-items: center; gap: 10px; padding: 0 12px; background: #111827; border-bottom: 1px solid #334155; }}
    #sim {{ width: 100%; height: calc(100% - 38px); border: 0; display: block; }}
    button {{ background: #2563eb; color: white; border: 0; border-radius: 4px; padding: 5px 10px; }}
    small {{ color: #94a3b8; }}
  </style>
</head>
<body>
  <div id="bar">
    <strong>ESP-Claw LVGL Lua Preview</strong>
    <button id="rerun">重新运行</button>
    <button id="stop">停止</button>
    <small>{name}</small>
  </div>
  <iframe id="sim" src="{sim_src}"></iframe>
  <script>
    const iframe = document.getElementById("sim");
    const lua = {script_json};
    const scriptPath = {script_path_json};
    window.__espClawPreviewLogs = [];
    window.__espClawPreviewEvents = [];
    window.__espClawPreviewReady = false;
    window.__espClawPreviewRunning = false;
    window.__espClawPreviewStartedAt = 0;
    window.__espClawPreviewExited = null;
    window.addEventListener("message", (event) => {{
      const data = event.data || {{}};
      if (!data.type || !String(data.type).startsWith("esp-claw-sim:")) {{
        return;
      }}
      window.__espClawPreviewEvents.push(data);
      if (data.type === "esp-claw-sim:log") {{
        window.__espClawPreviewLogs.push(String(data.message || ""));
      }} else if (data.type === "esp-claw-sim:ready") {{
        window.__espClawPreviewReady = true;
      }} else if (data.type === "esp-claw-sim:running") {{
        window.__espClawPreviewRunning = true;
        window.__espClawPreviewStartedAt = Date.now();
        window.__espClawPreviewExited = null;
      }} else if (data.type === "esp-claw-sim:exited") {{
        window.__espClawPreviewRunning = false;
        window.__espClawPreviewExited = Object.assign({{}}, data, {{
          runtimeMs: Math.max(0, Date.now() - (window.__espClawPreviewStartedAt || Date.now()))
        }});
      }} else if (data.type === "esp-claw-sim:error") {{
        window.__espClawPreviewLogs.push("[err] " + String(data.error || ""));
      }}
    }});
    function post(type, extra) {{
      iframe.contentWindow.postMessage(Object.assign({{ type }}, extra || {{}}), "*");
    }}
    function mountAndRun() {{
      post("esp-claw-sim:setResolution", {{ width: 800, height: 480 }});
      post("esp-claw-sim:mountSkill", {{
        skill: {{
          id: "host-preview",
          root: "/uploads",
          entry: scriptPath,
          files: [{{ path: scriptPath, text: lua }}],
          peripherals: ["lcd_touch"]
        }}
      }});
      setTimeout(() => post("esp-claw-sim:runSkill", {{ path: scriptPath }}), 250);
    }}
    iframe.addEventListener("load", () => setTimeout(mountAndRun, 300));
    document.getElementById("rerun").addEventListener("click", mountAndRun);
    document.getElementById("stop").addEventListener("click", () => post("esp-claw-sim:stop"));
  </script>
</body>
</html>"""


def run_web_lvgl_check(
    script: str,
    *,
    name: str = "preview.lua",
    expects_long_run: bool = True,
    min_runtime_s: float = 5.0,
    timeout_s: float = 8.0,
) -> WebLvglCheckResult:
    """Run the real WebAssembly simulator headlessly and collect runtime logs.

    This is a preflight check. It intentionally skips when artifacts or
    Playwright are unavailable so normal device iteration remains usable.
    """
    artifacts = find_web_lvgl_sim()
    if artifacts is None:
        return WebLvglCheckResult(ok=True, skipped=True, reason="未找到 Web LVGL 模拟器构建产物")
    try:
        from playwright.sync_api import TimeoutError as PlaywrightTimeoutError
        from playwright.sync_api import sync_playwright
    except Exception as err:  # pragma: no cover - depends on local optional dep
        return WebLvglCheckResult(ok=True, skipped=True, reason=f"未安装 Playwright：{err}")

    server = WebLvglPreviewServer(artifacts)
    url = server.set_script(script, name)
    console_lines: list[str] = []
    screenshot_path = ""
    visual_summary = ""
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            page = browser.new_page(viewport={"width": 900, "height": 560})
            page.on("console", lambda msg: console_lines.append(f"[browser:{msg.type}] {msg.text}"))
            page.goto(url, wait_until="domcontentloaded", timeout=int(timeout_s * 1000))
            page.wait_for_function("window.__espClawPreviewReady === true", timeout=int(timeout_s * 1000))
            try:
                page.wait_for_function(
                    "window.__espClawPreviewExited !== null",
                    timeout=int(max(min_runtime_s, timeout_s) * 1000),
                )
            except PlaywrightTimeoutError:
                pass
            logs = page.evaluate("window.__espClawPreviewLogs || []") or []
            exited = page.evaluate("window.__espClawPreviewExited") or None
            running = bool(page.evaluate("window.__espClawPreviewRunning === true"))
            runtime_s = 0.0
            exit_code = None
            if isinstance(exited, dict):
                runtime_s = float(exited.get("runtimeMs") or 0) / 1000.0
                try:
                    exit_code = int(exited.get("code"))
                except (TypeError, ValueError):
                    exit_code = None
            elif running:
                runtime_s = max(min_runtime_s, timeout_s)
            sim_frame = next((frame for frame in page.frames if "/sim/" in frame.url), None)
            if sim_frame is not None:
                safe_name = "".join(ch if ch.isalnum() or ch in ".-_" else "_" for ch in name)
                out_dir = Path(tempfile.gettempdir()) / "esp_claw_web_previews"
                out_dir.mkdir(parents=True, exist_ok=True)
                screenshot = out_dir / f"{int(time.time())}-{safe_name}.png"
                sim_frame.locator("#canvas").screenshot(path=str(screenshot), timeout=2000)
                screenshot_path = str(screenshot)
                visual = sim_frame.evaluate(
                    """() => {
                      const c = document.getElementById("canvas");
                      if (!c) return { available: false, reason: "canvas missing" };
                      const ctx = c.getContext("2d");
                      if (!ctx) return { available: false, reason: "2d context unavailable" };
                      const w = c.width || 0, h = c.height || 0;
                      if (!w || !h) return { available: false, reason: "canvas has no size" };
                      const data = ctx.getImageData(0, 0, w, h).data;
                      const step = Math.max(4, Math.floor(data.length / 4 / 4096) * 4);
                      const colors = new Set();
                      let sampled = 0, nonBlack = 0, bright = 0;
                      for (let i = 0; i < data.length; i += step) {
                        const r = data[i], g = data[i + 1], b = data[i + 2], a = data[i + 3];
                        if (a > 0) {
                          sampled++;
                          if (r + g + b > 24) nonBlack++;
                          if (r + g + b > 384) bright++;
                          colors.add(`${r >> 4},${g >> 4},${b >> 4}`);
                        }
                      }
                      return {
                        available: true,
                        sampled,
                        nonBlackRatio: sampled ? nonBlack / sampled : 0,
                        brightRatio: sampled ? bright / sampled : 0,
                        uniqueColors: colors.size,
                      };
                    }"""
                )
                if isinstance(visual, dict) and visual.get("available"):
                    non_black = float(visual.get("nonBlackRatio") or 0)
                    bright = float(visual.get("brightRatio") or 0)
                    unique = int(visual.get("uniqueColors") or 0)
                    visual_summary = (
                        f"visual=non_black:{non_black:.3f},bright:{bright:.3f},colors:{unique}"
                    )
                elif isinstance(visual, dict):
                    visual_summary = f"visual={visual.get('reason') or 'unavailable'}"
            if running:
                page.evaluate("document.getElementById('stop').click()")
            browser.close()
    except Exception as err:  # pragma: no cover - browser/environment dependent
        return WebLvglCheckResult(
            ok=True,
            skipped=True,
            reason=f"Web 预览自动检查不可用：{err}",
            log="\n".join(console_lines[-80:]),
            url=url,
        )
    finally:
        server.stop()

    combined = "\n".join([*(str(item) for item in logs), *console_lines])
    error_match = re_search_runtime_error(combined)
    if error_match:
        return WebLvglCheckResult(
            ok=False,
            reason=error_match,
            log=combined,
            url=url,
            exit_code=exit_code,
            runtime_s=runtime_s,
            screenshot_path=screenshot_path,
            visual_summary=visual_summary,
        )
    if exit_code not in (None, 0):
        return WebLvglCheckResult(
            ok=False,
            reason=f"模拟器退出码 {exit_code}",
            log=combined,
            url=url,
            exit_code=exit_code,
            runtime_s=runtime_s,
            screenshot_path=screenshot_path,
            visual_summary=visual_summary,
        )
    if expects_long_run and exited is not None and runtime_s < min_runtime_s:
        return WebLvglCheckResult(
            ok=False,
            reason=f"常驻脚本在 Web 预览中 {runtime_s:.1f}s 就退出",
            log=combined,
            url=url,
            exit_code=exit_code,
            runtime_s=runtime_s,
            screenshot_path=screenshot_path,
            visual_summary=visual_summary,
        )
    if visual_summary.startswith("visual=non_black:"):
        parts = dict(
            item.split(":", 1)
            for item in visual_summary.removeprefix("visual=").split(",")
            if ":" in item
        )
        non_black = float(parts.get("non_black", "0") or 0)
        unique = int(parts.get("colors", "0") or 0)
        if non_black < 0.01 and unique <= 2:
            return WebLvglCheckResult(
                ok=False,
                reason="Web 预览画面疑似空白",
                log=combined,
                url=url,
                exit_code=exit_code,
                runtime_s=runtime_s,
                screenshot_path=screenshot_path,
                visual_summary=visual_summary,
            )
    return WebLvglCheckResult(
        ok=True,
        log=combined,
        url=url,
        exit_code=exit_code,
        runtime_s=runtime_s,
        screenshot_path=screenshot_path,
        visual_summary=visual_summary,
    )


def re_search_runtime_error(text: str) -> str:
    for line in (text or "").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if (
            "Lua error:" in stripped
            or stripped.startswith("[err]")
            or stripped.startswith("ERROR:")
            or "bad argument" in stripped
            or "attempt to " in stripped
        ):
            return stripped[:240]
    return ""

#!/usr/bin/env python3
"""「我的」花园 · 同源 Lua/LVGL 仿真驱动（被 gd.py 调用，也可单独 import）。

仿真产物来自固件仓库 tools/lua_lvgl_web_sim 的 WebAssembly 构建（真实 LVGL 9.5 + Lua 5.4 C 绑定），
本模块只读使用。查找顺序与主题外壳 xj.py 一致：
  1. 环境变量 ESP_CLAW_LVGL_SIM_DIR
  2. ~/.cache/backclip-web/esp_claw/build/lua_lvgl_web_sim（主题外壳验证用的那一份）
  3. ~/.cache/backclip-web/build

按键：仿真里的 button 模块是桩，按键用 autoplay.json 时间轴注入（Lua 侧 core/input.lua 读取）。
触摸：Playwright 在画布上真实点按/拖动，走 LVGL 的指针输入设备。
日志约定：GD_MARK <名字> 触发截图（Lua 侧随后顺延 700 ms），GD_ERR / GD_FATAL / [err] 计为失败。
"""
import functools
import http.server
import json
import os
import pathlib
import re
import socketserver
import threading
import time

HOME = pathlib.Path.home()
SIM_CANDIDATES = [
    os.environ.get("ESP_CLAW_LVGL_SIM_DIR"),
    str(HOME / ".cache/backclip-web/esp_claw/build/lua_lvgl_web_sim"),
    str(HOME / ".cache/backclip-web/build"),
]
ERR_RE = re.compile(r"GD_ERR|GD_FATAL|\[err\]|lua error|traceback|PANIC", re.I)


def sim_dir():
    for c in SIM_CANDIDATES:
        if c and (pathlib.Path(c) / "esp_claw_sim.wasm").exists():
            return pathlib.Path(c)
    raise SystemExit("找不到仿真构建（esp_claw_sim.wasm），请设置 ESP_CLAW_LVGL_SIM_DIR")


PAGE = """<!doctype html><meta charset="utf-8"><title>garden sim</title>
<style>html,body{margin:0;background:#111}#sim{width:%(fw)dpx;height:%(fh)dpx;border:0;display:block}</style>
<iframe id="sim" src="/sim/esp_claw_sim.html?embedded=1"></iframe>
<script>
const files = %(files)s, entry = %(entry)s;
window.__logs = []; window.__ready = false; window.__exited = null; window.__running = false;
addEventListener("message", e => {
  const d = e.data || {}; if (!d.type || !String(d.type).startsWith("esp-claw-sim:")) return;
  if (d.type === "esp-claw-sim:log") window.__logs.push(String(d.message || ""));
  else if (d.type === "esp-claw-sim:ready") window.__ready = true;
  else if (d.type === "esp-claw-sim:running") window.__running = true;
  else if (d.type === "esp-claw-sim:exited") { window.__running = false; window.__exited = d; }
  else if (d.type === "esp-claw-sim:error") window.__logs.push("[err] " + String(d.error || ""));
});
const sim = document.getElementById("sim");
const post = (type, extra) => sim.contentWindow.postMessage(Object.assign({ type }, extra || {}), "*");
window.__write = (path, text) => sim.contentWindow.espClawSim.mountText(path, text);
sim.addEventListener("load", () => setTimeout(() => {
  post("esp-claw-sim:setResolution", { width: %(w)d, height: %(h)d });
  post("esp-claw-sim:mountSkill", { skill: { id: "my_garden", root: "/system/skills/my_garden", entry, files, peripherals: ["lcd_touch"] } });
  setTimeout(() => post("esp-claw-sim:runSkill", { path: entry }), 400);
}, 300));
</script>"""


def serve(page_html, root):
    class H(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.split("?")[0] in ("/", "/index.html"):
                body = page_html.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path.startswith("/sim/"):
                rel = self.path.split("?")[0][5:]
                p = (root / rel).resolve()
                if root.resolve() in p.parents and p.exists():
                    data = p.read_bytes()
                    ctype = {".html": "text/html", ".js": "application/javascript", ".wasm": "application/wasm"}.get(p.suffix, "application/octet-stream")
                    self.send_response(200)
                    self.send_header("Content-Type", ctype)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
            self.send_error(404)

    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def chromium(p):
    try:
        return p.chromium.launch()
    except Exception:
        import glob
        hits = sorted(glob.glob(os.path.expanduser("~/Library/Caches/ms-playwright/chromium-*/chrome-mac*/*.app/Contents/MacOS/*")))
        return p.chromium.launch(executable_path=hits[-1])


def run(files, entry, out, seconds=25.0, touches=None, size=(480, 800), record_ms=0):
    """files: [{path, text}]；touches: [{t, do: tap|drag|shot|write|long, ...}]。返回 summary 字典。"""
    from playwright.sync_api import sync_playwright
    root = sim_dir()
    w, h = size
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.png"):
        old.unlink()
    touches = sorted(touches or [], key=lambda a: a["t"])
    page = PAGE % {"files": json.dumps(files, ensure_ascii=False), "entry": json.dumps(entry), "w": w, "h": h, "fw": w + 16, "fh": h + 16}
    srv = serve(page, root)
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    logs, shots, errors = [], [], []
    t_start = time.time()
    with sync_playwright() as p:
        browser = chromium(p)
        pg = browser.new_page(viewport={"width": w + 40, "height": h + 40}, device_scale_factor=1)
        pg.goto(url)
        pg.wait_for_function("window.__running === true || window.__exited !== null", timeout=60000)
        t0 = time.time()
        canvas = pg.frame_locator("#sim").locator("canvas").first
        seen, ti = 0, 0
        frames, next_frame = [], 0.0
        if record_ms:
            (out / "frames").mkdir(exist_ok=True)
            for old in (out / "frames").glob("f*.png"):
                old.unlink()

        def grab(lines):
            for line in lines:
                logs.append(line)
                m = re.search(r"GD_MARK (\S+)", line)
                if m:
                    time.sleep(0.15)  # 等这一帧刷到屏上；Lua 侧在截图点会顺延后续按键 700 ms
                    path = out / f"{m.group(1)}.png"
                    canvas.screenshot(path=str(path))
                    shots.append(path.name)
                if ERR_RE.search(line):
                    errors.append(line)

        while time.time() - t0 < seconds + 5:
            if record_ms and time.time() - t0 >= next_frame:
                fp = out / "frames" / f"f{len(frames):04d}.png"
                canvas.screenshot(path=str(fp))
                frames.append(fp)
                next_frame = time.time() - t0 + record_ms / 1000
            new = pg.evaluate("window.__logs.slice(%d)" % seen)
            seen += len(new)
            grab(new)
            elapsed_ms = (time.time() - t0) * 1000
            while ti < len(touches) and elapsed_ms >= touches[ti]["t"]:
                a = touches[ti]
                box = canvas.bounding_box()
                sx, sy = box["width"] / w, box["height"] / h
                X = lambda v: box["x"] + v * sx
                Y = lambda v: box["y"] + v * sy
                if a["do"] == "tap":
                    pg.mouse.click(X(a["x"]), Y(a["y"]))
                elif a["do"] == "long":
                    pg.mouse.move(X(a["x"]), Y(a["y"]))
                    pg.mouse.down()
                    time.sleep(a.get("ms", 900) / 1000)
                    pg.mouse.up()
                elif a["do"] == "drag":
                    pg.mouse.move(X(a["x"]), Y(a["y"]))
                    pg.mouse.down()
                    steps = a.get("steps", 12)
                    for k in range(1, steps + 1):
                        pg.mouse.move(X(a["x"] + (a["x2"] - a["x"]) * k / steps), Y(a["y"] + (a["y2"] - a["y"]) * k / steps))
                        time.sleep(a.get("step_ms", 20) / 1000)
                    pg.mouse.up()
                elif a["do"] == "shot":
                    canvas.screenshot(path=str(out / f"{a['name']}.png"))
                    shots.append(a["name"] + ".png")
                elif a["do"] == "write":
                    # 运行中改写仿真文件系统里的文件（模拟手机/电脑把新植物推到 DATA，用于热重载验证）
                    pg.evaluate("([p, t]) => window.__write(p, t)", [a["path"], a["text"]])
                    logs.append(f"[host] wrote {a['path']} ({len(a['text'])} chars)")
                ti += 1
            if pg.evaluate("window.__exited !== null"):
                grab(pg.evaluate("window.__logs.slice(%d)" % seen))  # 退出后再收一次，避免漏记收尾行
                break
            time.sleep(0.04)
        browser.close()
    srv.shutdown()
    (out / "log.txt").write_text("\n".join(logs), "utf-8")
    if record_ms and frames:
        import subprocess
        fps = max(1, round(len(frames) / max(1e-3, time.time() - t0)))
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(fps), "-i", str(out / "frames" / "f%04d.png"),
                        "-vf", "scale=320:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer:bayer_scale=4",
                        "-loop", "0", str(out / "record.gif")], check=False)
    states = [l for l in logs if "GD_STATE" in l]
    perf = [l for l in logs if "GD_PERF" in l]
    summary = {
        "sim": str(root), "wall_s": round(time.time() - t_start, 1), "shots": shots, "errors": errors[:30],
        "states": len(states), "last_state": states[-1] if states else None, "perf": perf,
        "done": any("GD_DONE" in l for l in logs), "fatal": any("GD_FATAL" in l for l in logs),
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), "utf-8")
    return summary

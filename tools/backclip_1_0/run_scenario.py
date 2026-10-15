#!/usr/bin/env python3
"""磁吸背夹 1.0 实机场景执行器（UART clip_cmd）。

模拟用户的正常输入：实体键（ui_key）、触摸（ui_touch）、等待、截图（screen_capture）、
状态断言（home_state / mode_get / apps_all ...）。每一步的结果、截图与设备日志都落盘，
用于自己验收和交给 Codex 判定。

场景 JSON：
{
  "name": "home_basic",
  "steps": [
    {"note": "从首页打开应用总览"},
    {"op": "ui_key", "key": "SET", "action": "click"},
    {"wait": 900},
    {"op": "home_state", "expect": {"view": "overview"}},
    {"shot": "overview"},
    {"op": "ui_touch", "type": "tap", "x": 128, "y": 386},
    {"op": "app_stop"}
  ]
}
"expect" 是子集匹配（字典逐键比较，列表要求等长逐项匹配）；"expect_contains": {"字段": "子串"} 做包含判断。

用法：
  run_scenario.py scenario.json [more.json ...] --out DIR [--port /dev/cu.usbserial-10]
"""
import argparse
import base64
import importlib.util
import json
import struct
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
CLI = HERE.parents[1] / "application/edge_agent/components/live_apps/tests/device_cli.py"


def load_device_module():
    spec = importlib.util.spec_from_file_location("device_cli", CLI)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_png(path, w, h, pixels):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = pixels[y * w * 2:(y + 1) * w * 2]
        for (v,) in struct.iter_unpack("<H", row):
            raw.extend(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))

    def chunk(k, d):
        return struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def capture(dev, path, scale=1):
    cap = dev.rpc({"op": "screen_capture", "scale": scale, "encoding": "rle"})
    enc = bytearray()
    for off in range(0, int(cap["bytes"]), 8192):
        r = dev.rpc({"op": "screen_read", "capture_id": cap["capture_id"], "offset": off, "max_bytes": 8192})
        enc.extend(base64.b64decode(r["data"]))
    pix = bytearray()
    if cap["format"] == "rle565le":
        for n, value in struct.iter_unpack("<HH", enc):
            pix.extend(struct.pack("<H", value) * n)
    else:
        pix = enc
    w, h = int(cap["width"]), int(cap["height"])
    if len(pix) != w * h * 2:
        raise RuntimeError(f"capture size mismatch {len(pix)} != {w * h * 2}")
    write_png(path, w, h, pix)
    dev.rpc({"op": "screen_release"})
    return {"width": w, "height": h, "colors": len(set(struct.iter_unpack("<H", bytes(pix[::97])))) }


def subset(expect, actual):
    if isinstance(expect, dict):
        if not isinstance(actual, dict):
            return False
        return all(k in actual and subset(v, actual[k]) for k, v in expect.items())
    if isinstance(expect, list):
        return isinstance(actual, list) and len(expect) == len(actual) and all(
            subset(e, a) for e, a in zip(expect, actual))
    return expect == actual


def speech_file(text, voice="Tingting"):
    import subprocess, tempfile, hashlib as _h
    cache = Path(tempfile.gettempdir()) / ("bc_say_" + _h.sha1((voice + text).encode()).hexdigest()[:12] + ".aiff")
    if not cache.exists():
        subprocess.run(["say", "-v", voice, "-o", str(cache), text], check=True, timeout=30)
    return cache


def run(dev, scenario, out):
    name = scenario.get("name", "scenario")
    # 先把要“说”的话都合成好，避免合成耗时挤占按住说话的录音窗口
    for st in scenario.get("steps", []):
        if "play" in st:
            speech_file(st["play"], st.get("voice", "Tingting"))
    sdir = out / name
    sdir.mkdir(parents=True, exist_ok=True)
    report = {"name": name, "steps": [], "passed": 0, "failed": 0}
    if scenario.get("reset", True):
        # 每个场景从干净的首页开始：先退出前台应用，再收起面板/编辑/语音卡片
        try:
            st = dev.rpc({"op": "home_state"})
            if st.get("exclusive_app"):
                dev.rpc({"op": "app_stop"})
                time.sleep(1.5)
            dev.rpc({"op": "home_reset"})
            time.sleep(0.6)
        except RuntimeError as e:
            report["reset_error"] = str(e)
    for i, step in enumerate(scenario.get("steps", [])):
        rec = {"i": i, "step": step}
        try:
            if "note" in step and len(step) == 1:
                rec["note"] = step["note"]
            elif "wait" in step:
                time.sleep(step["wait"] / 1000.0)
            elif "play" in step:
                # 用电脑扬声器说一句话（macOS say 合成），模拟用户对背夹说话；不阻塞后续步骤
                import subprocess
                text = step["play"]
                cache = speech_file(text, step.get("voice", "Tingting"))
                if step.get("delay_ms"):
                    time.sleep(step["delay_ms"] / 1000.0)
                subprocess.Popen(["afplay", str(cache)])
                rec["played"] = text
            elif "shell" in step:
                # 同一个串口会话里操作手机等外部工具（打开串口会复位背夹，蓝牙连接要在会话内重建）
                import subprocess
                cp = subprocess.run(step["shell"], shell=True, capture_output=True, text=True,
                                    timeout=step.get("timeout_ms", 60000) / 1000.0, cwd=str(HERE))
                rec["shell_rc"] = cp.returncode
                rec["shell_out"] = (cp.stdout + cp.stderr)[-600:]
                if step.get("expect_rc", 0) is not None and cp.returncode != step.get("expect_rc", 0):
                    report["failed"] += 1
                    print(f"  [FAIL] step {i}: shell rc={cp.returncode} {rec['shell_out'][-200:]}", flush=True)
            elif "wait_state" in step:
                # 轮询 home_state 直到字段匹配（子集），或超时
                want = step["wait_state"]
                deadline = time.time() + step.get("timeout_ms", 30000) / 1000.0
                last = None
                while time.time() < deadline:
                    last = dev.rpc({"op": "home_state"})
                    if subset(want, last):
                        break
                    time.sleep(1.0)
                ok = last is not None and subset(want, last)
                rec["result"] = last
                rec["ok"] = ok
                report["passed" if ok else "failed"] += 1
                if not ok:
                    print(f"  [FAIL] step {i}: wait_state {want} -> {json.dumps(last, ensure_ascii=False)[:300]}", flush=True)
            elif "shot" in step:
                png = sdir / f"{i:02d}_{step['shot']}.png"
                rec["shot"] = str(png)
                rec["capture"] = capture(dev, png, step.get("scale", 1))
            else:
                body = {k: v for k, v in step.items() if k not in ("expect", "expect_contains", "allow_error", "note")}
                try:
                    res = dev.rpc(body)
                    rec["result"] = res
                    ok = True
                except RuntimeError as e:
                    res = {"error": str(e)}
                    rec["result"] = res
                    ok = bool(step.get("allow_error"))
                if "expect" in step:
                    ok = ok and subset(step["expect"], res)
                for key, sub in step.get("expect_contains", {}).items():
                    ok = ok and sub in json.dumps(res.get(key, ""), ensure_ascii=False)
                rec["ok"] = ok
                if "expect" in step or "expect_contains" in step:
                    report["passed" if ok else "failed"] += 1
                if not ok:
                    print(f"  [FAIL] step {i}: {json.dumps(step, ensure_ascii=False)[:160]} -> "
                          f"{json.dumps(res, ensure_ascii=False)[:300]}", flush=True)
        except Exception as e:  # noqa: BLE001 记录并继续，后续步骤仍有价值
            rec["exception"] = repr(e)
            report["failed"] += 1
            print(f"  [EXC] step {i}: {e!r}", flush=True)
        report["steps"].append(rec)
    (sdir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenarios", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--port", default="/dev/cu.usbserial-10")
    args = ap.parse_args()
    mod = load_device_module()
    args.out.mkdir(parents=True, exist_ok=True)
    dev = mod.Device(args.port)
    summary = []
    try:
        info = None
        for attempt in range(6):
            # 刚开机时日志刷屏、各服务还在起，首条命令可能收不到回复：等一等再试
            try:
                info = dev.rpc({"op": "device_info"})
                break
            except RuntimeError:
                time.sleep(4)
        if info is None:
            raise RuntimeError("device_info 连续无响应")
        (args.out / "device_info.json").write_text(json.dumps(info, ensure_ascii=False, indent=2))
        print("DEVICE", info.get("version"), info.get("mode"), flush=True)
        for path in args.scenarios:
            for sc in (json.loads(path.read_text()) if path.read_text().strip().startswith("[")
                       else [json.loads(path.read_text())]):
                print("SCENARIO", sc.get("name"), flush=True)
                rep = run(dev, sc, args.out)
                summary.append({"name": rep["name"], "passed": rep["passed"], "failed": rep["failed"]})
                print(f"  -> passed {rep['passed']} failed {rep['failed']}", flush=True)
    finally:
        (args.out / "uart.log").write_text("\n".join(dev.log))
        dev.close()
    (args.out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2))
    failed = sum(s["failed"] for s in summary)
    print("SUMMARY", json.dumps(summary, ensure_ascii=False), flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

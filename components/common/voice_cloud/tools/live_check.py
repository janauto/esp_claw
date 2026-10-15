#!/usr/bin/env python3
"""真实云端核验：macOS say 合成中文 → ffmpeg(libopus) 编成背夹同参数 Opus（16 kHz 单声道 24 kbps 60 ms）
→ 按包喂给组件公开 API（voice_cloud_begin/push_opus/finish）→ 百炼 qwen3-asr-flash + DeepSeek（工具调用）。

    python3 components/common/voice_cloud/tools/live_check.py --out /path/to/scratch/live
    python3 .../live_check.py --out DIR --env ~/.config/backclip-voice/secrets.env --dashscope-chat qwen-plus

- C 驱动 tools/vc_live.c 用 ASan/UBSan + libcurl 编译，HTTP 之外的代码与固件同一份；
- 密钥只由 vc_live 从 env 文件读进内存交给组件；本脚本也会读一次，仅用于检查所有输出与转储里
  都不含密钥，绝不打印、不写文件；
- --out 目录里保存：合成语音、帧文件、每次请求的正文（不含密钥）、事件流和汇总 live_results.json。
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parent
ROOT = COMPONENT.parents[2]
DEFAULT_ENV = Path.home() / ".config/backclip-voice/secrets.env"
DEFAULT_CJSON = ROOT / "application/edge_agent/managed_components/espressif__cjson/cJSON"
DEFAULT_FONT = ROOT / "application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf"
SOURCES = ["vc_ogg.c", "vc_text.c", "vc_proto.c", "vc_pipeline.c", "voice_cloud.c"]

VOICE_CASES = [
    # id, 合成的话, 期望
    ("brightness", "背夹，帮我把亮度调到百分之五十",
     {"heard": ["背夹", "亮度"], "tools": [("set_brightness", {"percent": 50})]}),
    ("volume_mode", "背夹，把音量调到百分之三十，然后切换到底座模式",
     {"heard": ["音量"], "tools": [("set_volume", {"percent": 30}), ("set_mode", {"mode": "dock"})]}),
    ("open_app", "打开天气应用",
     {"heard": ["天气"], "tools": [("open_app", None)]}),
    ("chitchat", "今天适合出去跑步吗",
     {"heard": ["跑步"], "tools": []}),
]
TEXT_CASES = [
    ("text_joke", "背夹，给我讲一个很短的笑话", {"tools": []}),
]


def load_env_values(path: Path) -> list[str]:
    """只为检查泄漏读取密钥值；不打印、不返回给调用者以外的地方。"""
    values = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip().removeprefix("export ").strip() in ("DASHSCOPE_API_KEY", "DEEPSEEK_API_KEY", "RELAY_TOKEN"):
            value = value.strip().strip('"').strip("'")
            if len(value) >= 8:
                values.append(value)
    return values


def build(out: Path, cjson: Path, tmp: Path) -> None:
    flags = ["-std=gnu11", "-g", "-O1", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
    cj = tmp / "cjson_live.o"
    subprocess.run(["clang", *flags, "-w", "-c", str(cjson / "cJSON.c"), "-o", str(cj)], check=True)
    srcs = [str(COMPONENT / "src" / s) for s in SOURCES]
    srcs += [str(COMPONENT / "test_host/stubs/host_stubs.c"), str(HERE / "vc_live.c")]
    subprocess.run(["clang", *flags, "-Wall", "-Wextra", "-Werror", "-Wno-gnu-zero-variadic-macro-arguments",
                    "-I" + str(COMPONENT / "include"), "-I" + str(COMPONENT / "src"),
                    "-I" + str(COMPONENT / "test_host/stubs"), "-I" + str(cjson), *srcs, str(cj),
                    "-lcurl", "-lpthread", "-o", str(out)], check=True)


def demux(data: bytes) -> list[bytes]:
    out, pos, partial = [], 0, b""
    while pos < len(data):
        assert data[pos:pos + 4] == b"OggS"
        nseg = data[pos + 26]
        lacing = data[pos + 27:pos + 27 + nseg]
        body = pos + 27 + nseg
        for v in lacing:
            partial += data[body:body + v]
            body += v
            if v < 255:
                if not partial.startswith((b"OpusHead", b"OpusTags")):
                    out.append(partial)
                partial = b""
        pos = body
    return out


def synth(text: str, workdir: Path) -> tuple[Path, int, list[int]]:
    """say → aiff → ffmpeg libopus（与 relay/test_relay.py 的 speech_ogg 相同参数）→ 帧文件。"""
    aiff = workdir / "speech.aiff"
    ogg = workdir / "speech.ogg"
    subprocess.run(["say", "-v", "Tingting", text, "-o", str(aiff)], check=True)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(aiff), "-ar", "16000", "-ac", "1",
                    "-c:a", "libopus", "-b:a", "24k", "-frame_duration", "60", "-application", "voip",
                    str(ogg)], check=True)
    packets = demux(ogg.read_bytes())
    frames = workdir / "speech.frames"
    frames.write_bytes(b"".join(struct.pack("<H", len(p)) + p for p in packets))
    return frames, len(packets), [len(p) for p in packets]


def run_live(binary: Path, env: Path, extra: list[str], dump: Path) -> tuple[list[dict], str]:
    dump.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(binary), "--env", str(env), "--dump", str(dump), *extra],
                       capture_output=True, text=True, timeout=180)
    lines = []
    for line in p.stdout.splitlines():
        try:
            lines.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return lines, p.stdout + p.stderr


def check_case(lines: list[dict], expect: dict) -> tuple[bool, list[str]]:
    notes = []
    ok = True
    events = [e for e in lines if e.get("event") in ("HEARD", "TOOL", "REPLY", "ERROR")]
    heard = next((e["text"] for e in events if e["event"] == "HEARD"), "")
    for word in expect.get("heard", []):
        if word not in heard:
            ok = False
            notes.append(f"识别结果缺少“{word}”")
    tools = [(e["text"], json.loads(e["detail"] or "{}")) for e in events if e["event"] == "TOOL"]
    for name, args in expect.get("tools", []):
        match = [t for t in tools if t[0] == name and (args is None or all(t[1].get(k) == v for k, v in args.items()))]
        if not match:
            ok = False
            notes.append(f"缺少工具调用 {name} {args}")
    if not expect.get("tools") and tools:
        notes.append(f"出现了未预期的工具调用 {tools}（不算失败）")
    reply = next((e for e in events if e["event"] in ("REPLY", "ERROR")), None)
    if not reply or reply["event"] != "REPLY" or not reply["text"]:
        ok = False
        notes.append(f"没有得到回复：{reply}")
    elif len(reply["text"].encode()) > 300:
        ok = False
        notes.append("回复超过 300 字节")
    return ok, notes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--env", type=Path, default=DEFAULT_ENV)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--cjson", type=Path, default=DEFAULT_CJSON)
    ap.add_argument("--font", type=Path, default=DEFAULT_FONT)
    ap.add_argument("--dashscope-chat", metavar="MODEL", help="另外用百炼兼容模式跑一次对话（例如 qwen-plus）")
    args = ap.parse_args()
    for tool in ("say", "ffmpeg", "ffprobe", "clang"):
        if not shutil.which(tool):
            print(f"缺少 {tool}")
            return 2
    if not args.env.is_file():
        print("缺少 env 文件")
        return 2
    secrets = load_env_values(args.env)
    args.out.mkdir(parents=True, exist_ok=True)
    binary = args.out / "vc_live"
    build(binary, args.cjson, args.out)

    try:
        from fontTools.ttLib import TTFont  # noqa: WPS433
        cmap = TTFont(str(args.font)).getBestCmap()
    except Exception:  # noqa: BLE001
        cmap = None

    results = []
    all_output = []
    for case_id, phrase, expect in VOICE_CASES:
        work = args.out / case_id
        work.mkdir(parents=True, exist_ok=True)
        frames, npk, sizes = synth(phrase, work)
        lines, raw = run_live(binary, args.env, ["--frames", str(frames)], work / "requests")
        all_output.append(raw)
        ok, notes = check_case(lines, expect)
        # ASR 请求体解码回 Ogg，ffprobe 检查
        req0_path = work / "requests/req_0.json"
        if not req0_path.is_file():
            results.append({"id": case_id, "mode": "voice", "phrase": phrase, "ok": False,
                            "notes": notes + ["没有发出 ASR 请求"], "events": lines})
            print("FAIL " + case_id, "没有发出 ASR 请求")
            continue
        req0 = json.loads(req0_path.read_text())
        data = req0["messages"][-1]["content"][0]["input_audio"]["data"]
        ogg = base64.b64decode(data.split(",", 1)[1])
        (work / "asr_payload.ogg").write_bytes(ogg)
        probe = json.loads(subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,channels", "-show_entries",
             "format=duration", "-of", "json", str(work / "asr_payload.ogg")],
            capture_output=True, text=True, check=True).stdout)
        dur = float(probe["format"]["duration"])
        if probe["streams"][0]["codec_name"] != "opus" or abs(dur - npk * 0.06) > 0.1:
            ok = False
            notes.append(f"ASR 载荷异常 {probe}")
        reply = next((e["text"] for e in lines if e.get("event") == "REPLY"), "")
        missing = sorted({ch for ch in reply if cmap is not None and ord(ch) not in cmap})
        results.append({
            "id": case_id, "mode": "voice", "phrase": phrase, "ok": ok, "notes": notes,
            "packets": npk, "packet_bytes": [min(sizes), max(sizes), round(sum(sizes) / len(sizes))],
            "audio_ms": npk * 60, "ogg_bytes": len(ogg), "asr_body_bytes": len(json.dumps(req0)),
            "events": [e for e in lines if e.get("event") not in ("CONFIG",)],
            "reply_glyphs_missing": "".join(missing),
        })
        print(("PASS " if ok else "FAIL ") + case_id, json.dumps(
            {k: v for k, v in results[-1].items() if k in ("packets", "audio_ms", "notes")}, ensure_ascii=False))
        time.sleep(1)

    # 热词 A/B：同一段语音，带/不带热词只做识别
    frames = args.out / "brightness/speech.frames"
    ab = {}
    for label, extra in (("with_context", []), ("no_context", ["--no-context"])):
        lines, raw = run_live(binary, args.env, ["--frames", str(frames), "--asr-only", *extra],
                              args.out / f"ab_{label}")
        all_output.append(raw)
        ab[label] = next((e["text"] for e in lines if e.get("event") == "HEARD"), None)
        time.sleep(1)
    results.append({"id": "hotword_ab", "mode": "asr_only", "ok": bool(ab.get("with_context")) and
                    "背夹" in ab.get("with_context", ""), "heard": ab})
    print("hotword A/B:", json.dumps(ab, ensure_ascii=False))

    for case_id, text, expect in TEXT_CASES:
        lines, raw = run_live(binary, args.env, ["--text", text], args.out / case_id)
        all_output.append(raw)
        ok, notes = check_case(lines, expect)
        results.append({"id": case_id, "mode": "text", "phrase": text, "ok": ok, "notes": notes,
                        "events": [e for e in lines if e.get("event") != "CONFIG"]})
        print(("PASS " if ok else "FAIL ") + case_id, notes)

    if args.dashscope_chat:
        lines, raw = run_live(binary, args.env, ["--frames", str(frames), "--chat-dashscope", args.dashscope_chat],
                              args.out / "dashscope_chat/requests")
        all_output.append(raw)
        ok, notes = check_case(lines, VOICE_CASES[0][2])
        req1_path = args.out / "dashscope_chat/requests/req_1.json"
        req1 = json.loads(req1_path.read_text()) if req1_path.is_file() else {}
        if req1.get("enable_thinking") is not False or "thinking" in req1:
            ok = False
            notes.append("百炼对话请求没有带 enable_thinking=false")
        results.append({"id": "dashscope_chat", "mode": "voice", "model": args.dashscope_chat, "ok": ok,
                        "notes": notes, "events": [e for e in lines if e.get("event") != "CONFIG"]})
        print(("PASS " if ok else "FAIL ") + "dashscope_chat", notes)

    # 泄漏检查：所有输出与转储文件都不得含密钥
    leaked = 0
    blobs = list(all_output)
    for path in args.out.rglob("*"):
        if path.is_file() and path.suffix in (".json", ".txt", ".log"):
            blobs.append(path.read_text(errors="replace"))
    for blob in blobs:
        for secret in secrets:
            if secret in blob:
                leaked += 1
    results.append({"id": "no_secret_in_outputs", "ok": leaked == 0 and len(secrets) > 0,
                    "checked_blobs": len(blobs), "secrets_checked": len(secrets)})
    print(("PASS " if leaked == 0 else "FAIL ") + f"no secret in {len(blobs)} outputs/dumps")

    (args.out / "live_results.json").write_text(json.dumps(results, ensure_ascii=False, indent=1), encoding="utf-8")
    bad = [r["id"] for r in results if not r["ok"]]
    print(f"\n{len(results) - len(bad)}/{len(results)} live checks passed; details: {args.out / 'live_results.json'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

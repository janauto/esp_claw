#!/usr/bin/env python3
"""编译并运行 voice_cloud 宿主测试（macOS clang；Linux 需去掉 leaks 一步）。

    python3 components/common/voice_cloud/test_host/run_tests.py
    python3 .../run_tests.py --ref-ogg /path/to/relay/ogg_opus.py   # 指定语音中继的封装实现做逐字节比对

步骤：
1. ASan + UBSan 构建 test_voice_cloud，跑全部单元测试（纯逻辑、假传输流水线、pthread 桩上的公开 API）；
   同时以 VC_LOG=1 运行，确认日志里不出现测试用的密钥；
2. TSan 构建再跑一遍（公开 API 的多线程路径）；
3. 不带 sanitizer 的构建放在 macOS `leaks --atExit` 下跑，检查泄漏；
4. Ogg 交叉核对：与语音中继 ogg_opus.py 逐字节比对；用 ffmpeg(libogg) 写出的真实页复算 CRC；
   ffmpeg/libopus 编出背夹同参数 Opus（16 kHz 单声道 24 kbps 60 ms）后由 C 封装，再交给 ffprobe；
   C 生成的完整 ASR 请求体解析回来，base64 解码后与 C 封装逐字节一致且 ffprobe 可读；
5. 给用户看的固定中文与清洗用到的替换字逐字检查是否都在设备字库里（需要 fontTools）。
只在临时目录里编译，不写组件目录和固件 build 目录。
"""
from __future__ import annotations

import argparse
import base64
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parent
ROOT = COMPONENT.parents[2]
DEFAULT_CJSON = ROOT / "application/edge_agent/managed_components/espressif__cjson/cJSON"
DEFAULT_FONT = ROOT / "application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf"
DEFAULT_REF = Path.home() / ".cache/backclip-1.0-refs/asr-voice-integration-e8596e/嵌入式/voice-asr/relay/ogg_opus.py"
SOURCES = ["vc_ogg.c", "vc_text.c", "vc_proto.c", "vc_pipeline.c", "voice_cloud.c"]
TEST_KEYS = ("sk-asr-TEST", "sk-chat-TEST")
WARN = ["-Wall", "-Wextra", "-Wshadow", "-Wformat=2", "-Werror", "-Wno-gnu-zero-variadic-macro-arguments"]

results: list[tuple[str, bool, str]] = []


def record(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""), flush=True)


def build(out: Path, cjson: Path, sanitize: list[str], tmp: Path, extra: list[str] | None = None) -> None:
    """cJSON 单独编译（第三方代码，关掉告警），其余源文件按 -Werror 编译。"""
    flags = ["-std=gnu11", "-g", "-O1", "-fno-omit-frame-pointer", *sanitize]
    cj_obj = tmp / (out.name + "_cjson.o")
    subprocess.run(["clang", *flags, "-w", "-c", str(cjson / "cJSON.c"), "-o", str(cj_obj)], check=True)
    srcs = [str(COMPONENT / "src" / s) for s in SOURCES]
    srcs += [str(HERE / "stubs/host_stubs.c"), str(HERE / "test_voice_cloud.c")]
    subprocess.run(["clang", *flags, *WARN, *(extra or []),
                    "-I" + str(COMPONENT / "include"), "-I" + str(COMPONENT / "src"),
                    "-I" + str(HERE / "stubs"), "-I" + str(cjson), *srcs, str(cj_obj),
                    "-lpthread", "-o", str(out)], check=True)


def run(cmd: list[str], env: dict | None = None, timeout: int = 300) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)


def fake_packets(n: int) -> list[bytes]:
    # 与 relay/test_relay.py 的 fake_packets 相同
    return [bytes([0x48]) + bytes((i * 7 + j) & 0xFF for j in range(40 + i)) for i in range(n)]


def pack_frames(packets: list[bytes]) -> bytes:
    return b"".join(struct.pack("<H", len(p)) + p for p in packets)


def demux(data: bytes) -> list[bytes]:
    """Ogg → 裸包（跳过 OpusHead/OpusTags），独立实现，只给测试用。"""
    out, pos, partial = [], 0, b""
    while pos < len(data):
        assert data[pos:pos + 4] == b"OggS", f"bad capture at {pos}"
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


def load_ref(path: Path):
    spec = importlib.util.spec_from_file_location("ref_ogg_opus", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


def ffprobe(path: Path) -> dict:
    p = run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,channels,sample_rate",
             "-show_entries", "format=duration", "-of", "json", str(path)])
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip())
    return json.loads(p.stdout)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cjson", type=Path, default=Path(os.environ.get("APP_TEST_CJSON", DEFAULT_CJSON)))
    ap.add_argument("--ref-ogg", type=Path, default=DEFAULT_REF)
    ap.add_argument("--font", type=Path, default=DEFAULT_FONT)
    ap.add_argument("--skip-tsan", action="store_true")
    ap.add_argument("--skip-leaks", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="voice-cloud-test-") as tmpdir:
        tmp = Path(tmpdir)

        # 1. ASan + UBSan
        asan = tmp / "test_asan"
        build(asan, args.cjson, ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], tmp)
        env = dict(os.environ, ASAN_OPTIONS="abort_on_error=0:detect_stack_use_after_return=1",
                   UBSAN_OPTIONS="print_stacktrace=1")
        p = run([str(asan)], env=env)
        record("unit tests (ASan+UBSan)", p.returncode == 0, (p.stdout.strip().splitlines() or ["?"])[-1])
        if p.returncode:
            print(p.stdout[-4000:], p.stderr[-8000:])
        env_log = dict(env, VC_LOG="1")
        p = run([str(asan)], env=env_log)
        text = p.stdout + p.stderr
        leaked = [k for k in TEST_KEYS if k in text]
        log_lines = sum(1 for line in p.stderr.splitlines() if "voice_cloud" in line)
        record("logs contain no keys (VC_LOG=1)", p.returncode == 0 and not leaked and log_lines > 0,
               f"{log_lines} log lines, leaked={leaked}")

        # 2. TSan
        if not args.skip_tsan:
            tsan = tmp / "test_tsan"
            build(tsan, args.cjson, ["-fsanitize=thread"], tmp)
            p = run([str(tsan)], env=dict(os.environ, TSAN_OPTIONS="halt_on_error=1"))
            record("unit tests (TSan)", p.returncode == 0 and "WARNING: ThreadSanitizer" not in p.stderr,
                   (p.stdout.strip().splitlines() or ["?"])[-1])
            if p.returncode:
                print(p.stderr[-8000:])

        # 3. leaks
        if not args.skip_leaks and shutil.which("leaks"):
            plain = tmp / "test_plain"
            build(plain, args.cjson, [], tmp)
            p = run(["leaks", "--atExit", "--", str(plain)])
            summary = next((line.strip() for line in p.stdout.splitlines() if "leaks for" in line), "?")
            record("no leaks (macOS leaks --atExit)", p.returncode == 0, summary)
            if p.returncode:
                print(p.stdout[-6000:])

        # 4. Ogg 交叉核对
        if args.ref_ogg.is_file():
            ref = load_ref(args.ref_ogg)
            cases = {
                "fake40": fake_packets(40),
                "fake1": fake_packets(1),
                "fake17": fake_packets(17),
                "lacing": [bytes([0x48]) + bytes(600), bytes([0x48]) + bytes(254)],
            }
            same = 0
            for name, pkts in cases.items():
                fr = tmp / f"{name}.frames"
                fr.write_bytes(pack_frames(pkts))
                out = tmp / f"{name}.ogg"
                run([str(asan), "--mux", str(fr), str(out), "backclip-voice-relay"], env=env)
                ok = out.is_file() and out.read_bytes() == ref.mux(pkts)
                same += ok
                if not ok:
                    print("mismatch:", name)
            record("Ogg bytes identical to relay ogg_opus.py", same == len(cases), f"{same}/{len(cases)} cases")
        else:
            record("Ogg bytes identical to relay ogg_opus.py", False, f"reference not found: {args.ref_ogg}")

        if shutil.which("ffmpeg") and shutil.which("ffprobe"):
            lib = tmp / "libogg.ogg"
            subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=f=440:d=1", "-ar", "16000",
                            "-ac", "1", "-c:a", "libopus", str(lib)], check=True)
            p = run([str(asan), "--crc-check", str(lib)], env=env)
            record("CRC matches ffmpeg/libogg pages", p.returncode == 0, p.stdout.strip())

            real = tmp / "real.ogg"
            subprocess.run(["ffmpeg", "-loglevel", "error", "-f", "lavfi", "-i", "sine=f=440:d=3", "-ar", "16000",
                            "-ac", "1", "-c:a", "libopus", "-b:a", "24k", "-frame_duration", "60",
                            "-application", "voip", str(real)], check=True)
            pkts = demux(real.read_bytes())
            fr = tmp / "real.frames"
            fr.write_bytes(pack_frames(pkts))
            mux = tmp / "real_c.ogg"
            run([str(asan), "--mux", str(fr), str(mux)], env=env)
            info = ffprobe(mux)
            dur = float(info["format"]["duration"])
            ok = info["streams"][0]["codec_name"] == "opus" and info["streams"][0]["channels"] == 1 and \
                abs(dur - len(pkts) * 0.06) < 0.1 and demux(mux.read_bytes()) == pkts
            record("ffprobe reads C-muxed real Opus", ok,
                   f"{len(pkts)} packets, {dur:.2f} s, {min(map(len, pkts))}-{max(map(len, pkts))} B/packet")
            if args.ref_ogg.is_file():
                run([str(asan), "--mux", str(fr), str(mux), "backclip-voice-relay"], env=env)
                record("real Opus mux identical to relay", mux.read_bytes() == load_ref(args.ref_ogg).mux(pkts))

            body = tmp / "asr_body.json"
            p = run([str(asan), "--asr-body", str(fr), str(body)], env=env)
            req = json.loads(body.read_text())
            data = req["messages"][1]["content"][0]["input_audio"]["data"]
            ogg = base64.b64decode(data.split(",", 1)[1], validate=True)
            run([str(asan), "--mux", str(fr), str(mux)], env=env)
            (tmp / "decoded.ogg").write_bytes(ogg)
            info2 = ffprobe(tmp / "decoded.ogg")
            ok = data.startswith("data:audio/ogg;base64,") and ogg == mux.read_bytes() and \
                req["model"] == "qwen3-asr-flash" and req["stream"] is False and \
                req["asr_options"] == {"enable_itn": True, "language": "zh"} and \
                "背夹" in req["messages"][0]["content"][0]["text"] and info2["streams"][0]["codec_name"] == "opus"
            record("ASR request body decodes to the same Ogg", ok, f"{body.stat().st_size} bytes body")
        else:
            record("ffmpeg/ffprobe checks", False, "ffmpeg or ffprobe missing")

        # 5. 字库覆盖
        try:
            from fontTools.ttLib import TTFont  # noqa: WPS433
        except ImportError:
            record("user strings covered by device font", False, "fontTools not installed")
        else:
            cmap = TTFont(str(args.font)).getBestCmap()
            p = run([str(asan), "--user-strings"], env=env)
            strings = [s for s in p.stdout.splitlines() if s]
            strings.append("！度华氏度->约不等于欧元英镑￥…，1.")  # 清洗时会写出的替换字
            missing = sorted({ch for s in strings for ch in s if ord(ch) not in cmap})
            record("user strings covered by device font", not missing and len(strings) > 10,
                   f"{len(strings)} strings, {len(cmap)} glyphs, missing={''.join(missing)}")

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

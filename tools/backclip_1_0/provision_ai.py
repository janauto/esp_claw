#!/usr/bin/env python3
"""把底座模式的 AI 服务配置写进背夹（UART，一次性）。

从受保护的 env 文件（默认 ~/.config/backclip-voice/secrets.env，权限 600）读取：
  DASHSCOPE_BASE_URL / DASHSCOPE_API_KEY  → 语音识别（百炼 qwen3-asr-flash）
  DEEPSEEK_API_KEY                         → 对话（DeepSeek deepseek-flash）
密钥只经 USB 串口发给设备，不打印、不写日志；设备侧只回显是否就绪与主机名。
用法：provision_ai.py [--port /dev/cu.usbserial-10] [--env 路径] [--chat-model deepseek-flash]
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
CLI = HERE.parents[1] / "application/edge_agent/components/live_apps/tests/device_cli.py"


def read_env(path):
    vals = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        vals[k.strip()] = v.strip().strip('"').strip("'")
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-10")
    ap.add_argument("--env", default=os.path.expanduser("~/.config/backclip-voice/secrets.env"))
    ap.add_argument("--chat-model", default="deepseek-flash")
    args = ap.parse_args()
    env = read_env(args.env)
    need = ["DASHSCOPE_BASE_URL", "DASHSCOPE_API_KEY", "DEEPSEEK_API_KEY"]
    missing = [k for k in need if not env.get(k)]
    if missing:
        raise SystemExit(f"env 文件缺少：{', '.join(missing)}")
    spec = importlib.util.spec_from_file_location("device_cli", CLI)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    dev = mod.Device(args.port)
    try:
        body = {
            "op": "ai_config_set",
            "asr_base": env["DASHSCOPE_BASE_URL"].rstrip("/"),
            "asr_key": env["DASHSCOPE_API_KEY"],
            "asr_model": "qwen3-asr-flash",
            "chat_base": "https://api.deepseek.com",
            "chat_key": env["DEEPSEEK_API_KEY"],
            "chat_model": args.chat_model,
        }
        try:
            res = dev.rpc(body)
        except Exception:  # noqa: BLE001 异常文本里可能带串口回显（含密钥），不能原样打印
            raise SystemExit("写入失败：设备无响应或返回错误（已隐藏串口回显）") from None
        # 只输出不含密钥的公开字段
        public = {k: v for k, v in res.items() if "key" not in k.lower()}
        print("AI_CONFIG", json.dumps(public, ensure_ascii=False))
        print("AI_CONFIG_GET", json.dumps(dev.rpc({"op": "ai_config_get"}), ensure_ascii=False))
    finally:
        dev.log.clear()      # 串口回显里有密钥：不保留
        dev.close()


if __name__ == "__main__":
    main()

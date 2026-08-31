#!/usr/bin/env python3
"""
ESP-Claw 电脑端 Vibe Coding 上位机。

职责边界：
- 运行在电脑端，不嵌入设备端 http_server。
- LLM 配置、API Key、对话历史保存在电脑本地。
- 设备端只通过 BLE bridge 接收命令、Lua 脚本和返回执行结果。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "7b7d1001-0000-0000-0057-414c43505345"
RX_UUID = "7b7d1002-0000-0000-0057-414c43505345"
TX_UUID = "7b7d1003-0000-0000-0057-414c43505345"
DEFAULT_DEVICE_PREFIX = "esp-claw"
REQUEST_CHUNK_SIZE = 64
DEFAULT_TIMEOUT = 15.0

APP_DIR = Path.home() / ".esp_claw" / "host_vibe_coding"
DEFAULT_CONFIG_PATH = APP_DIR / "config.json"
DEFAULT_HISTORY_PATH = APP_DIR / "history.jsonl"
REPO_ROOT = Path(__file__).resolve().parents[2]
LUA_MODULES_DIR = REPO_ROOT / "components" / "lua_modules"
LUA_MODULE_DOC_LIMIT = 6000


LUA_MODULE_DOCS: dict[str, dict[str, tuple[str, ...]]] = {
    "display": {
        "keywords": (
            "显示",
            "屏幕",
            "界面",
            "lcd",
            "display",
            "lvgl",
            "ui",
            "按钮",
            "文本",
            "画",
            "绘制",
        ),
        "paths": (
            "lua_module_display/README.md",
            "lua_module_lvgl/README.md",
        ),
    },
}


@dataclass(frozen=True)
class HostLlmConfig:
    provider: str = "custom"
    base_url: str = "https://api.openai.com/v1"
    model: str = "gpt-4o-mini"
    api_key: str = ""
    auth_type: str = "bearer"
    max_tokens: int = 4096
    system_prompt: str = "你是 ESP-Claw 电脑端上位机的 Lua 生成器。"


@dataclass(frozen=True)
class HostLlmPreset:
    label: str
    base_url: str
    auth_type: str
    model: str
    max_tokens: int = 4096


HOST_LLM_PRESETS: dict[str, HostLlmPreset] = {
    "openai": HostLlmPreset("OpenAI", "https://api.openai.com/v1", "bearer", "gpt-4o-mini"),
    "bailian": HostLlmPreset("阿里百炼", "https://dashscope.aliyuncs.com/compatible-mode/v1", "bearer", "qwen-plus"),
    "deepseek": HostLlmPreset("DeepSeek", "https://api.deepseek.com", "bearer", "deepseek-chat"),
    "kimi_global": HostLlmPreset("Kimi（Global）", "https://api.moonshot.ai/v1", "bearer", "kimi-k2.6"),
    "kimi_cn": HostLlmPreset("Kimi（CN）", "https://api.moonshot.cn/v1", "bearer", "kimi-k2.6"),
    "openai_compatible": HostLlmPreset(
        "OpenAI 兼容 API",
        "https://api.openai.com/v1",
        "bearer",
        "gpt-4o-mini",
    ),
    "custom": HostLlmPreset("自定义", "", "bearer", ""),
}

HOST_LLM_PRESET_ORDER: tuple[str, ...] = (
    "openai",
    "bailian",
    "deepseek",
    "kimi_global",
    "kimi_cn",
    "openai_compatible",
    "custom",
)


def provider_label(provider: str) -> str:
    preset = HOST_LLM_PRESETS.get(provider)
    return preset.label if preset else provider


def provider_values() -> list[str]:
    return [HOST_LLM_PRESETS[key].label for key in HOST_LLM_PRESET_ORDER if key in HOST_LLM_PRESETS]


def provider_key_from_label(label: str) -> str:
    for key in HOST_LLM_PRESET_ORDER:
        preset = HOST_LLM_PRESETS.get(key)
        if preset and preset.label == label:
            return key
    return "custom"


def preset_for_provider(provider: str) -> HostLlmPreset | None:
    return HOST_LLM_PRESETS.get(provider)


def infer_provider(config: HostLlmConfig) -> str:
    base_url = config.base_url.rstrip("/")
    for key, preset in HOST_LLM_PRESETS.items():
        if key == "custom":
            continue
        if preset.base_url.rstrip("/") == base_url and preset.auth_type == config.auth_type:
            return key
    return "custom"


def apply_provider_preset(config: HostLlmConfig, provider: str, *, keep_model: bool = False) -> HostLlmConfig:
    preset = preset_for_provider(provider)
    if not preset:
        return HostLlmConfig(
            provider="custom",
            base_url=config.base_url,
            model=config.model,
            api_key=config.api_key,
            auth_type=config.auth_type,
            max_tokens=config.max_tokens,
            system_prompt=config.system_prompt,
        )

    model = config.model if keep_model and config.model else preset.model
    return HostLlmConfig(
        provider=provider,
        base_url=preset.base_url or config.base_url,
        model=model,
        api_key=config.api_key,
        auth_type=preset.auth_type,
        max_tokens=config.max_tokens or preset.max_tokens,
        system_prompt=config.system_prompt,
    )


def read_json(path: Path, fallback: dict[str, Any]) -> dict[str, Any]:
    if not path.exists():
        return fallback
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return {**fallback, **data} if isinstance(data, dict) else fallback
    except json.JSONDecodeError:
        return fallback


def load_config(path: Path) -> HostLlmConfig:
    env_api_key = os.getenv("ESP_CLAW_HOST_LLM_API_KEY", "")
    data = read_json(path, asdict(HostLlmConfig()))
    if env_api_key:
        data["api_key"] = env_api_key
    config = HostLlmConfig(
        provider=str(data.get("provider") or "custom"),
        base_url=str(data.get("base_url") or "").rstrip("/"),
        model=str(data.get("model") or ""),
        api_key=str(data.get("api_key") or ""),
        auth_type=str(data.get("auth_type") or "bearer"),
        max_tokens=int(data.get("max_tokens") or 4096),
        system_prompt=str(data.get("system_prompt") or HostLlmConfig.system_prompt),
    )
    inferred = infer_provider(config)
    if config.provider in HOST_LLM_PRESETS and config.provider != "custom":
        return config
    return HostLlmConfig(
        provider=inferred,
        base_url=config.base_url,
        model=config.model,
        api_key=config.api_key,
        auth_type=config.auth_type,
        max_tokens=config.max_tokens,
        system_prompt=config.system_prompt,
    )



def save_config(path: Path, config: HostLlmConfig) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(asdict(config), ensure_ascii=False, indent=2), encoding="utf-8")


def append_history(path: Path, item: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fp:
        fp.write(json.dumps(item, ensure_ascii=False) + "\n")


def read_recent_history(path: Path, limit: int = 8) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    lines = path.read_text(encoding="utf-8").splitlines()[-limit:]
    out: list[dict[str, Any]] = []
    for line in lines:
        try:
            item = json.loads(line)
            if isinstance(item, dict):
                out.append(item)
        except json.JSONDecodeError:
            continue
    return out


def request_id(prefix: str) -> str:
    return f"{prefix}-{int(time.time() * 1000):x}"


def parse_json_maybe(text: str) -> Any:
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


class BleBridge:
    def __init__(self, device: Any | None, name_prefix: str, timeout: float) -> None:
        self.device = device
        self.name_prefix = name_prefix
        self.timeout = timeout
        self.client: BleakClient | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._pending: dict[str, asyncio.Future[dict[str, Any]]] = {}
        self._response_chunks: dict[str, dict[str, Any]] = {}

    async def __aenter__(self) -> "BleBridge":
        await self.connect()
        return self

    async def __aexit__(self, exc_type: object, exc: object, tb: object) -> None:
        await self.disconnect()

    async def connect(self) -> None:
        target = self.device or await self._discover_device()
        self.client = BleakClient(target)
        await self.client.connect(timeout=self.timeout)
        await self.client.start_notify(TX_UUID, self._on_notify)

    async def disconnect(self) -> None:
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        self._response_chunks.clear()

    async def _discover_device(self) -> str:
        devices = await BleakScanner.discover(timeout=self.timeout, service_uuids=[SERVICE_UUID])
        matches = [item for item in devices if (item.name or "").startswith(self.name_prefix)]
        if not matches:
            names = ", ".join(filter(None, (item.name for item in devices))) or "无可见 BLE 设备"
            raise RuntimeError(f"未找到 {self.name_prefix}* 设备。扫描结果：{names}")
        return str(matches[0].address)

    def _on_notify(self, _: int, data: bytearray) -> None:
        text = bytes(data).decode("utf-8", errors="replace")
        try:
            frame = json.loads(text)
        except json.JSONDecodeError:
            frame = {"id": "", "ok": True, "type": "raw", "data": text}

        kind = str(frame.get("kind") or frame.get("k") or "")
        if kind in {"response_chunk", "c"}:
            req_id = str(frame.get("id") or frame.get("i") or "")
            total = int(frame.get("total") or frame.get("t") or 0)
            seq = int(frame.get("seq") or frame.get("s") or 0)
            payload = str(frame.get("data") or frame.get("d") or "")
            current = self._response_chunks.setdefault(req_id, {"total": total, "parts": {}})
            current["total"] = total or int(current["total"])
            current["parts"][seq] = payload
            if current["total"] > 0 and len(current["parts"]) == current["total"]:
                joined = "".join(current["parts"][i] for i in range(current["total"]))
                self._response_chunks.pop(req_id, None)
                self._finish(json.loads(joined))
            return

        self._finish(frame)

    def _finish(self, frame: dict[str, Any]) -> None:
        req_id = str(frame.get("id") or "")
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(self._resolve_pending, req_id, frame)
        else:
            self._resolve_pending(req_id, frame)

    def _resolve_pending(self, req_id: str, frame: dict[str, Any]) -> None:
        future = self._pending.pop(req_id, None)
        if future and not future.done():
            future.set_result(frame)

    async def request(self, command: dict[str, Any], timeout: float | None = None) -> dict[str, Any]:
        if not self.client or not self.client.is_connected:
            raise RuntimeError("BLE bridge 未连接")
        req_id = str(command.get("id") or request_id("req"))
        command = {**command, "id": req_id}
        loop = asyncio.get_running_loop()
        self._loop = loop
        future: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[req_id] = future
        try:
            await self._send(command)
            return await asyncio.wait_for(future, timeout=timeout or self.timeout)
        except Exception:
            self._pending.pop(req_id, None)
            self._response_chunks.pop(req_id, None)
            if not future.done():
                future.cancel()
            raise

    async def _send(self, command: dict[str, Any]) -> None:
        assert self.client is not None
        text = json.dumps(command, ensure_ascii=False, separators=(",", ":"))
        if len(text.encode("utf-8")) <= REQUEST_CHUNK_SIZE:
            await self.client.write_gatt_char(RX_UUID, text.encode("utf-8"), response=False)
            return

        chunks = split_by_utf8_size(text, REQUEST_CHUNK_SIZE)
        for seq, data in enumerate(chunks):
            frame = {
                "i": command["id"],
                "k": "c",
                "o": command.get("op", ""),
                "s": seq,
                "t": len(chunks),
                "d": data,
            }
            payload = json.dumps(frame, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            await self.client.write_gatt_char(RX_UUID, payload, response=False)
            await asyncio.sleep(0.02)


def split_by_utf8_size(text: str, max_bytes: int) -> list[str]:
    chunks: list[str] = []
    current: list[str] = []
    current_size = 0
    for char in text:
        size = len(char.encode("utf-8"))
        if current and current_size + size > max_bytes:
            chunks.append("".join(current))
            current = []
            current_size = 0
        current.append(char)
        current_size += size
    if current:
        chunks.append("".join(current))
    return chunks


def call_openai_compatible(config: HostLlmConfig, messages: list[dict[str, str]]) -> str:
    if not config.base_url or not config.model:
        raise RuntimeError("请先配置 base_url 和 model")
    if config.auth_type != "none" and not config.api_key:
        raise RuntimeError("请先配置 API Key，或设置 auth_type=none")

    headers = {"Content-Type": "application/json"}
    if config.auth_type == "bearer":
        headers["Authorization"] = f"Bearer {config.api_key}"

    body = json.dumps(
        {
            "model": config.model,
            "max_tokens": config.max_tokens,
            "messages": messages,
        },
        ensure_ascii=False,
    ).encode("utf-8")
    request = urllib.request.Request(
        f"{config.base_url.rstrip('/')}/chat/completions",
        data=body,
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as err:
        detail = err.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"LLM 请求失败：HTTP {err.code} {detail}") from err

    content = payload.get("choices", [{}])[0].get("message", {}).get("content")
    if not isinstance(content, str) or not content.strip():
        raise RuntimeError("LLM 返回为空")
    return content


def extract_lua(text: str) -> str:
    match = re.search(r"```lua\s*([\s\S]*?)```", text, flags=re.IGNORECASE)
    return (match.group(1) if match else text).strip()


def select_lua_module_docs(prompt: str) -> list[Path]:
    haystack = prompt.lower()
    selected: list[Path] = []
    seen: set[Path] = set()
    for spec in LUA_MODULE_DOCS.values():
        keywords = spec.get("keywords", ())
        if not any(keyword.lower() in haystack for keyword in keywords):
            continue
        for rel_path in spec.get("paths", ()):
            path = LUA_MODULES_DIR / rel_path
            if path not in seen:
                selected.append(path)
                seen.add(path)
    return selected


def read_lua_module_context(prompt: str) -> str:
    sections: list[str] = []
    for path in select_lua_module_docs(prompt):
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        if len(text) > LUA_MODULE_DOC_LIMIT:
            text = text[:LUA_MODULE_DOC_LIMIT].rstrip() + "\n...[truncated]"
        sections.append(f"## {path.relative_to(REPO_ROOT)}\n{text}")
    return "\n\n".join(sections)


def validate_generated_lua(script: str) -> None:
    errors: list[str] = []
    single_return_vars = re.findall(r"local\s+(\w+)\s*=\s*[^\n]*get_display_lcd_params", script)
    if single_return_vars:
        errors.append("board_manager.get_display_lcd_params() 必须按多返回值接收，不能当作 table/userdata 访问")
        for var_name in single_return_vars:
            if re.search(rf"\b{re.escape(var_name)}\s*\.\s*(?:panel_handle|io_handle|panel_if|width|height|pixel_format)\b", script):
                errors.append("display_lcd 参数不能使用 .panel_handle/.width 这类字段访问")
    for call in re.findall(r"display\.draw_text_aligned\s*\(([^\n]*)\)", script):
        if call.count(",") < 5:
            errors.append("display.draw_text_aligned() 必须使用 x, y, width, height, text, options 六参数形态")
    if re.search(r"display\.(?:clear|fill_rect|draw_rect|draw_text|draw_text_aligned)\([^\n]*(?:0x[0-9a-fA-F]{3,8})", script):
        errors.append("display 颜色不能使用 0xRRGGBB 数字，必须使用颜色表、hex 字符串或命名颜色")
    if errors:
        raise RuntimeError("生成的 Lua 不符合设备 display API：" + "；".join(dict.fromkeys(errors)))


def build_messages(
    prompt: str,
    device_info: str,
    history: list[dict[str, Any]],
    system_prompt: str | None = None,
) -> list[dict[str, str]]:
    recent = []
    for item in history[-6:]:
        user = item.get("prompt")
        result = item.get("result")
        if isinstance(user, str):
            recent.append({"role": "user", "content": user})
        if isinstance(result, str):
            recent.append({"role": "assistant", "content": f"上次执行结果：\n{result[:2000]}"})

    lua_module_context = read_lua_module_context(prompt)

    messages = [
        {
            "role": "system",
            "content": (
                f"{system_prompt or '你是 ESP-Claw 电脑端上位机的 Lua 生成器。'}"
                "只生成可在设备 Lua runtime 执行的 Lua 脚本。"
                "不要请求、打印或保存 API Key。"
                "涉及设备能力时必须严格遵循随后的 Lua 模块 README 上下文，README 优先于通用经验。"
                "显示屏必须使用真实的 display 或 lvgl Lua 模块，不要虚构 screen capability。"
                "低层绘制优先使用 display；复杂控件界面才使用 lvgl；禁止在同一个脚本中同时 display.init 和 lvgl.init。"
                "display 初始化必须使用 board_manager.get_display_lcd_params(\"display_lcd\") 的多返回值，"
                "正确形式是 local panel_handle, io_handle, width, height, panel_if, pixel_format = board_manager.get_display_lcd_params(\"display_lcd\")；"
                "禁止把 get_display_lcd_params 的返回值当作 table/userdata 后再访问 .panel_handle、.width 等字段。"
                "display.init 必须调用 display.init(panel_handle, io_handle, width, height, panel_if, pixel_format)。"
                "display.draw_text 的正确形式是 display.draw_text(x, y, text, { color = ..., font_size = ... })。"
                "display.draw_text_aligned 的正确形式是 display.draw_text_aligned(x, y, width, height, text, { color = ..., font_size = ..., align = ..., valign = ... })。"
                "所有坐标、宽高、font_size 必须是整数，除法结果要用 // 或 math.floor。"
                "颜色必须使用颜色表 { r = 255, g = 255, b = 255 }、hex 字符串 \"#ffffff\" 或命名颜色 \"white\"；禁止使用 0xFFFFFF 这类数字颜色。"
                "绘制必须在 display.begin_frame(...) 与 display.present()/display.end_frame() 生命周期内完成。"
                "静态画面 present 后不要立刻 deinit 或退出，至少 delay.delay_ms(5000) 保持显示会话；否则画面可能只闪一下或被系统 UI 接管。"
                "清理用的 pcall 不能包住整个业务逻辑，也不能吞掉显示错误；业务失败必须通过 error 或 assert 返回。"
                "不要调用 display.set_font、display.text、display.refresh 或 capability.get。"
                "如需调用其他设备能力，优先使用 capability.call()。"
                "最终只返回一个 lua 代码块。"
            ),
        },
        {"role": "system", "content": f"设备上下文：\n{device_info or '(尚未获取)'}"},
    ]
    if lua_module_context:
        messages.append({"role": "system", "content": f"Lua 模块 README 上下文：\n{lua_module_context}"})
    messages.extend(recent)
    messages.append({"role": "user", "content": prompt})
    return messages


async def cmd_scan(args: argparse.Namespace) -> None:
    devices = await BleakScanner.discover(timeout=args.timeout, service_uuids=[SERVICE_UUID])
    for item in devices:
        print(f"{item.address}\t{item.name or '(unnamed)'}")


async def with_bridge(args: argparse.Namespace, fn: Callable[[BleBridge], Any]) -> Any:
    async with BleBridge(args.device, args.name_prefix, args.timeout) as bridge:
        return await fn(bridge)


async def cmd_hello(args: argparse.Namespace) -> None:
    async def run(bridge: BleBridge) -> None:
        result = await bridge.request({"id": request_id("hello"), "op": "hello"})
        print_json(result)

    await with_bridge(args, run)


async def cmd_info(args: argparse.Namespace) -> None:
    async def run(bridge: BleBridge) -> None:
        result = await bridge.request(
            {
                "id": request_id("info"),
                "op": "device_info",
            },
            timeout=args.timeout,
        )
        print_json(result)

    await with_bridge(args, run)


async def cmd_run_lua(args: argparse.Namespace) -> None:
    script = Path(args.script).read_text(encoding="utf-8")
    validate_generated_lua(script)

    async def run(bridge: BleBridge) -> None:
        put_id = request_id("lua")
        put = await bridge.request({"id": put_id, "op": "put_lua", "source": script}, timeout=args.timeout)
        if not put.get("ok"):
            raise RuntimeError(str(put.get("error") or put))
        path = str(put.get("data") or "")
        run_result = await bridge.request(
            {"id": request_id("run"), "op": "run_lua", "path": path, "timeout_ms": args.lua_timeout_ms},
            timeout=args.timeout,
        )
        print_json({"put_lua": put, "run_lua": run_result})

    await with_bridge(args, run)


async def cmd_ask(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    history = read_recent_history(args.history)

    async def run(bridge: BleBridge) -> None:
        info = await bridge.request(
            {
                "id": request_id("info"),
                "op": "device_info",
            },
            timeout=args.timeout,
        )
        device_info = str(info.get("data") or info.get("error") or "")
        raw = call_openai_compatible(config, build_messages(args.prompt, device_info, history))
        script = extract_lua(raw)
        validate_generated_lua(script)
        print("\n--- Generated Lua ---\n")
        print(script)
        print("\n--- End Lua ---\n")
        if not args.yes:
            answer = input("上传并执行？[y/N] ").strip().lower()
            if answer not in {"y", "yes"}:
                return

        put_id = request_id("lua")
        put = await bridge.request({"id": put_id, "op": "put_lua", "source": script}, timeout=args.timeout)
        if not put.get("ok"):
            raise RuntimeError(str(put.get("error") or put))
        run_result = await bridge.request(
            {
                "id": request_id("run"),
                "op": "run_lua",
                "path": str(put.get("data") or ""),
                "timeout_ms": args.lua_timeout_ms,
            },
            timeout=args.timeout,
        )
        append_history(
            args.history,
            {
                "created_at": int(time.time()),
                "prompt": args.prompt,
                "raw": raw,
                "script": script,
                "device_info": device_info,
                "result": run_result,
            },
        )
        print_json({"put_lua": put, "run_lua": run_result})

    await with_bridge(args, run)


def cmd_config(args: argparse.Namespace) -> None:
    current = load_config(args.config)
    provider = args.provider or current.provider
    preset = HOST_LLM_PRESETS.get(provider)
    next_config = HostLlmConfig(
        provider=provider,
        base_url=(args.base_url if args.base_url is not None else (preset.base_url if args.provider and preset else current.base_url)),
        model=(args.model if args.model is not None else (preset.model if args.provider and preset else current.model)),
        api_key=args.api_key if args.api_key is not None else current.api_key,
        auth_type=(args.auth_type if args.auth_type is not None else (preset.auth_type if args.provider and preset else current.auth_type)),
        max_tokens=(args.max_tokens if args.max_tokens is not None else (preset.max_tokens if args.provider and preset else current.max_tokens)),
        system_prompt=current.system_prompt,
    )
    save_config(args.config, next_config)
    safe = {**asdict(next_config), "api_key": "***" if next_config.api_key else ""}
    print_json(safe)


def print_json(value: Any) -> None:
    print(json.dumps(parse_result_data(value), ensure_ascii=False, indent=2))


def parse_result_data(value: Any) -> Any:
    if isinstance(value, dict):
        out = {key: parse_result_data(item) for key, item in value.items()}
        if isinstance(out.get("data"), str):
            out["data"] = parse_json_maybe(out["data"])
        return out
    if isinstance(value, list):
        return [parse_result_data(item) for item in value]
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ESP-Claw 电脑端 Vibe Coding 上位机")
    parser.add_argument("--device", help="BLE 地址；不传则按名称前缀扫描")
    parser.add_argument("--name-prefix", default=DEFAULT_DEVICE_PREFIX, help="BLE 设备名前缀")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="BLE/请求超时秒数")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH, help="电脑端 LLM 配置文件")
    parser.add_argument("--history", type=Path, default=DEFAULT_HISTORY_PATH, help="电脑端执行历史文件")

    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("scan", help="扫描 ESP-Claw BLE bridge")
    sub.add_parser("hello", help="BLE 握手")
    sub.add_parser("info", help="获取设备上下文")

    config = sub.add_parser("config", help="设置电脑端 LLM 配置")
    config.add_argument("--provider", choices=list(HOST_LLM_PRESETS.keys()))
    config.add_argument("--base-url")
    config.add_argument("--model")
    config.add_argument("--api-key")
    config.add_argument("--auth-type", choices=["bearer", "none"])
    config.add_argument("--max-tokens", type=int)

    ask = sub.add_parser("ask", help="自然语言生成 Lua，并通过 BLE 上传执行")
    ask.add_argument("prompt")
    ask.add_argument("--yes", action="store_true", help="跳过执行确认")
    ask.add_argument("--lua-timeout-ms", type=int, default=60000)

    run_lua = sub.add_parser("run-lua", help="直接上传并执行本地 Lua 文件")
    run_lua.add_argument("script")
    run_lua.add_argument("--lua-timeout-ms", type=int, default=60000)
    return parser


async def main_async(args: argparse.Namespace) -> None:
    if args.command == "scan":
        await cmd_scan(args)
    elif args.command == "hello":
        await cmd_hello(args)
    elif args.command == "info":
        await cmd_info(args)
    elif args.command == "ask":
        await cmd_ask(args)
    elif args.command == "run-lua":
        await cmd_run_lua(args)
    else:
        raise RuntimeError(f"未知命令：{args.command}")


def main() -> None:
    args = build_parser().parse_args()
    if args.command == "config":
        cmd_config(args)
        return
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        pass
    except Exception as err:
        print(f"ERROR: {err}", file=sys.stderr)
        raise SystemExit(1) from err


if __name__ == "__main__":
    main()
#!/usr/bin/env python3
"""
ESP-Claw 电脑端图形上位机。

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
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable

import host_context
import llm_client
import lua_module_catalog as catalog
from context_engine import BuiltContext, ContextRequest
from llm_client import (
    API_STYLE_ANTHROPIC,
    API_STYLE_OPENAI_CHAT,
    API_STYLES,
    Cancellation,
    Cancelled,
    LlmHttpError,
    LlmResult,
    StreamUnsupported,
    check_connection,
    resolve_endpoint_url,
)
from task_planner import TaskPlan, plan_task
from web_lvgl_preview import WebLvglCheckResult, run_web_lvgl_check

if TYPE_CHECKING:
    from bleak import BleakClient

SERVICE_UUID = "7b7d1001-0000-0000-0057-414c43505345"
RX_UUID = "7b7d1002-0000-0000-0057-414c43505345"
TX_UUID = "7b7d1003-0000-0000-0057-414c43505345"
DEFAULT_DEVICE_PREFIX = "esp-claw"
REQUEST_CHUNK_SIZE = 64
DEFAULT_TIMEOUT = 15.0

APP_DIR = Path.home() / ".esp_claw" / "host_vibe_coding"
DEFAULT_CONFIG_PATH = APP_DIR / "config.json"
DEFAULT_HISTORY_PATH = APP_DIR / "history.jsonl"
REPO_ROOT = catalog.REPO_ROOT

DEFAULT_MAX_ATTEMPTS = 3

#: 默认生成预算。按 deepseek-v4-pro 这类大窗口模型规划：一份完整的
#: LVGL 游戏脚本 200+ 行，4096 装不下，截断后上传的是半个程序（能过语法、秒退）。
DEFAULT_MAX_TOKENS = 32000
#: 默认上下文窗口（deepseek-v4-pro / v4-flash 均为 1M）。
DEFAULT_CONTEXT_WINDOW = 1_000_000

#: HTTP 读超时的下限，以及每 1000 个输出 token 追加的秒数。
#: 原先硬编码 90s：12000 token 的输出在慢一点的服务上根本写不完，
#: 表现就是 urllib 抛 read timeout，和模型、提示词都无关。
HTTP_TIMEOUT_MIN_S = 180.0
HTTP_TIMEOUT_PER_1K_TOKENS_S = 12.0
HTTP_TIMEOUT_MAX_S = 1800.0

#: 流式请求的"卡住"判定：多久没有收到任何新内容就认为连接断了。
#: 流式下超时的语义从"总耗时"变成"静默间隔"，所以这个值可以给得很小——
#: 模型写得慢不会触发，网络真的断了才会。
HTTP_STREAM_STALL_TIMEOUT_S = 90.0
#: 生成阶段的先后顺序，只许前进。数值本身没有意义，只用来比较。
STREAM_PHASE_RANK = {"": 0, "waiting": 1, "reasoning": 2, "content": 3}
#: 生成期间如果这么久没有可报的动作，就打一行"还活着"，附上已耗时。
#: 存在的意义是覆盖"思维链很长但没写代码"这段——否则界面会长时间静默。
STREAM_HEARTBEAT_S = 15.0
#: "哪些状态码值得改字段重试"现在由 llm_client.DEGRADABLE_STATUS 决定，
#: 转发一个别名给已有调用点。原来那套"400 就退回非流式"的粗判已经去掉：
#: 它对 max_completion_tokens 这类真正的字段不兼容无效，退回后同样的 400
#: 再来一次，而且第二次的错误会盖掉第一次真正有用的那条。
STREAM_UNSUPPORTED_HTTP_CODES = llm_client.DEGRADABLE_STATUS

JOB_POLL_INTERVAL = 1.0  # BLE 轮询间隔秒数（太长会让用户感觉卡顿）
#: job 状态长时间不变（长跑脚本停在 running）时的"还在跑"间隔。
JOB_HEARTBEAT_S = 15.0
JOB_POLL_TIMEOUT = 30.0  # 默认轮询超时，缩短到 30 秒提升响应速度
TERMINAL_JOB_STATUS = frozenset({"done", "failed", "timeout", "stopped"})
STOP_EXISTING_WAIT_AFTER_STOP_S = 6.0
STOP_EXISTING_SETTLE_S = 1.0

#: 一次性脚本的执行上限。跑完就该退回主界面，卡住时要有人来收尸。
DEFAULT_LUA_TIMEOUT_MS = 60000
#: 传给设备的 timeout_ms=0 在 cap_lua 异步路径里表示"跑到被取消为止"。
#: 游戏/常驻界面必须用这个值：给任何有限值都等于约好了几秒后把画面掐掉。
LUA_TIMEOUT_UNTIL_CANCELLED = 0
#: 界面/游戏脚本至少要跑这么久才算真的把画面撑住了，低于这个值属于"闪一下就退"。
LONG_RUN_MIN_S = 5
#: 完整游戏/界面脚本的生成预算下限。日志里的贪吃蛇三次都是被 4096 截断的半个程序。
#: 只在用户把 max_tokens 调到比这更低时才起作用（见 gen_budget 的 max()）。
LONG_RUN_MIN_TOKENS = 16000
#: 需要常驻主循环的任务类型，命中则用 expects_long_run 的判定方向。
LONG_RUN_TASK_KINDS = frozenset({"game", "ui", "animation", "interactive"})
LONG_RUN_PROMPT_HINTS = (
    "游戏", "贪吃蛇", "俄罗斯方块", "动画", "界面", "按钮", "菜单",
    "时钟", "表盘", "仪表", "触摸", "点击", "交互", "屏幕上",
)


def wants_long_run(prompt: str, task_kind: str = "") -> bool:
    """判断这次任务是否本该常驻运行。"""
    if (task_kind or "").strip().lower() in LONG_RUN_TASK_KINDS:
        return True
    return any(hint in (prompt or "") for hint in LONG_RUN_PROMPT_HINTS)


def resolve_lua_timeout_ms(requested: int | None, expects_long_run: bool) -> int:
    """决定这次执行给设备多长的执行窗口。

    requested 为 None 表示"按任务类型自动定"，这是默认路径：
    - 游戏/常驻界面 -> 0（跑到被取消为止）。给有限值会让设备到点把 job 杀掉，
      表现就是玩着玩着 status=timeout、画面退回主界面。
    - 一次性脚本 -> 有限上限。这类脚本本来就该执行完退回主界面，
      给 0 的话写错一个死循环就没人收尸了。

    显式传值（CLI/GUI 手动指定）一律照办，包括显式的 0。
    """
    if requested is not None:
        return max(0, requested)
    return LUA_TIMEOUT_UNTIL_CANCELLED if expects_long_run else DEFAULT_LUA_TIMEOUT_MS


FLICKER_DIAGNOSIS = (
    "脚本执行后 {runtime}s 就正常退出了，屏幕只闪了一下就回到主界面，设备没有任何日志输出。"
    "这类任务必须常驻运行：脚本末尾要有 `while true do ... lvgl.process_events(16) end` "
    "主循环，或用 lvgl 定时器 + 阻塞等待把脚本撑住。"
    "上一轮只创建了对象/定义了状态表就走到文件末尾，Lua 解释器随即退出，LVGL 对象跟着销毁。"
    "请输出带主循环的完整可运行脚本，不要只给片段。"
)

DEBUG_LUA_INSTRUCTIONS = """本轮生成的是 DEBUG 版脚本，用于先在上位机预览、再上传设备定位问题。

调试要求：
1. 在脚本开头定义 `local DEBUG = true` 和 `local function dbg(tag, value)`，dbg 内部用 `print("[DBG] " .. tag .. ": " .. tostring(value))` 输出。
2. 在初始化、模块 require、设备句柄获取、关键分支、事件回调、主循环启动、错误兜底处添加少量 dbg()。
3. dbg 内容要能定位问题，但不要在高频主循环每帧打印；循环里最多在状态变化、碰撞、按钮/触摸事件发生时打印。
4. 生成完整可运行脚本，不要只给片段。"""

RELEASE_LUA_INSTRUCTIONS = """本轮生成的是 RELEASE 版脚本。

发布要求：
1. 去除 DEBUG 标志、dbg() 辅助函数和所有调试打印。
2. 保留用户确认可用的功能逻辑、初始化顺序、错误处理和常驻主循环。
3. 不要为了精简而删除必要的 require、设备句柄获取、事件处理或防 nil 判断。"""


def generation_system_prompt(base_prompt: str | None, *, debug: bool = True) -> str:
    """给 GUI 的 debug/release 工作流追加生成约束。"""
    parts = [(base_prompt or "").strip()]
    parts.append(DEBUG_LUA_INSTRUCTIONS if debug else RELEASE_LUA_INSTRUCTIONS)
    return "\n\n".join(part for part in parts if part)


def strip_debug_instrumentation(script: str) -> str:
    """把按 DEBUG 约定生成的 dbg() 调试输出从脚本里剥掉。

    这不是 Lua parser，只处理上位机提示词要求模型生成的固定形态：
    `local DEBUG = true`、`local function dbg ... end` 和独立的 dbg(...) 调用。
    """
    lines = (script or "").splitlines()
    out: list[str] = []
    skipping_dbg_fn = False
    dbg_fn_depth = 0

    for line in lines:
        stripped = line.strip()
        if re.match(r"local\s+DEBUG\s*=", stripped):
            continue
        if re.match(r"(?:local\s+)?function\s+dbg\s*\(", stripped):
            if re.search(r"\bend\b\s*$", stripped):
                continue
            skipping_dbg_fn = True
            dbg_fn_depth = 1
            continue
        if skipping_dbg_fn:
            if re.search(r"\b(?:function|if|for|while|repeat)\b", stripped) and not stripped.startswith("--"):
                dbg_fn_depth += 1
            if stripped == "end":
                dbg_fn_depth -= 1
                if dbg_fn_depth <= 0:
                    skipping_dbg_fn = False
            continue
        if re.match(r"dbg\s*\(", stripped):
            continue
        out.append(line)

    # Collapse excessive blank lines left by removed debug blocks.
    compact: list[str] = []
    blank = 0
    for line in out:
        if line.strip():
            blank = 0
            compact.append(line)
        else:
            blank += 1
            if blank <= 1:
                compact.append(line)
    return "\n".join(compact).strip() + "\n"


def simulated_device_preview(
    prompt: str,
    plan: TaskPlan | ModulePlan | None,
    script: str,
    *,
    debug: bool = True,
    device_info: str = "",
) -> str:
    """生成上位机侧的“模拟设备端预览”，用于上传前人工确认。

    这里不执行 Lua，只把计划、模块、运行窗口、调试输出位置和潜在风险摊开。
    """
    modules = getattr(plan, "modules", []) if plan is not None else []
    task_kind = getattr(plan, "task_kind", "") if plan is not None else ""
    expects_long_run = wants_long_run(prompt, task_kind) or script_expects_long_run(script)
    timeout_ms = resolve_lua_timeout_ms(None, expects_long_run)
    require_names = re.findall(r"require\s*\(?\s*[\"']([\w.]+)[\"']", script or "")
    debug_calls = len(re.findall(r"^\s*dbg\s*\(", script or "", flags=re.MULTILINE))
    print_calls = len(re.findall(r"^\s*print\s*\(", script or "", flags=re.MULTILINE))

    lines = [
        "=== 模拟设备端预览 ===",
        f"状态：{'DEBUG' if debug else 'RELEASE'}",
        f"用户需求：{prompt}",
    ]
    if plan is not None:
        lines.append("")
        lines.append(getattr(plan, "describe", lambda: str(plan))())
    if device_info.strip():
        lines.append("")
        lines.append("设备信息：已作为生成上下文注入（预览区只显示摘要，不重复展开）。")
    lines.extend([
        "",
        "预计执行：",
        f"- require 模块：{', '.join(require_names) or '(未识别)'}",
        f"- 规划模块：{', '.join(modules) or '(无)'}",
        "- 运行窗口：" + ("不限时，需手动停止 job" if timeout_ms == LUA_TIMEOUT_UNTIL_CANCELLED else f"{timeout_ms // 1000}s"),
        f"- 调试输出：dbg={debug_calls} 处，print={print_calls} 处",
    ])
    if debug and debug_calls == 0:
        lines.append("- 风险：DEBUG 版没有 dbg()，设备端出错时定位信息可能不足。")
    try:
        validate_generated_lua(script)
        lines.append("- 本地上传前校验：通过")
    except RuntimeError as err:
        lines.append(f"- 本地上传前校验：失败，{err}")
    if expects_long_run and timeout_ms != LUA_TIMEOUT_UNTIL_CANCELLED:
        lines.append("- 风险：该需求像常驻界面/游戏，但执行窗口不是不限时。")
    return "\n".join(lines)


@dataclass(frozen=True)
class HostLlmConfig:
    provider: str = "deepseek"
    base_url: str = "https://api.deepseek.com/v1"
    model: str = "deepseek-v4-pro"
    api_key: str = ""
    auth_type: str = "bearer"
    max_tokens: int = DEFAULT_MAX_TOKENS
    system_prompt: str = "你是 ESP-Claw 电脑端上位机的 Lua 生成器。"
    #: 接口风格：openai_chat（/chat/completions）或 anthropic（/v1/messages）。
    #: 两者的请求体、鉴权头、SSE 事件形状完全不同，不能靠 base_url 临时猜。
    api_style: str = API_STYLE_OPENAI_CHAT
    #: 附加请求头。走自建网关时常要带 x-tenant 之类的东西。
    extra_headers: dict[str, str] = field(default_factory=dict)
    #: 附加请求体字段（temperature / top_p / reasoning_effort …）。
    #: 决定语义的字段（model/messages/stream/max_tokens）不允许在这里覆盖。
    extra_params: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class HostLlmPreset:
    label: str
    base_url: str
    auth_type: str
    model: str
    max_tokens: int = DEFAULT_MAX_TOKENS
    #: 该模型的上下文窗口，仅用于展示和体积预警，不参与请求体。
    context_window: int = 0
    api_style: str = API_STYLE_OPENAI_CHAT


HOST_LLM_PRESETS: dict[str, HostLlmPreset] = {
    "openai": HostLlmPreset("OpenAI", "https://api.openai.com/v1", "bearer", "gpt-4o-mini"),
    "bailian": HostLlmPreset("阿里百炼", "https://dashscope.aliyuncs.com/compatible-mode/v1", "bearer", "qwen-plus"),
    "deepseek": HostLlmPreset(
        "DeepSeek V4 Pro",
        "https://api.deepseek.com/v1",
        "bearer",
        "deepseek-v4-pro",
        max_tokens=DEFAULT_MAX_TOKENS,
        context_window=DEFAULT_CONTEXT_WINDOW,
    ),
    "deepseek_flash": HostLlmPreset(
        "DeepSeek V4 Flash",
        "https://api.deepseek.com/v1",
        "bearer",
        "deepseek-v4-flash",
        max_tokens=DEFAULT_MAX_TOKENS,
        context_window=DEFAULT_CONTEXT_WINDOW,
    ),
    "kimi_global": HostLlmPreset("Kimi（Global）", "https://api.moonshot.ai/v1", "bearer", "kimi-k2.6"),
    "kimi_cn": HostLlmPreset("Kimi（CN）", "https://api.moonshot.cn/v1", "bearer", "kimi-k2.6"),
    "anthropic": HostLlmPreset(
        "Anthropic（原生 Messages）",
        "https://api.anthropic.com/v1",
        "bearer",
        "claude-sonnet-5",
        max_tokens=DEFAULT_MAX_TOKENS,
        context_window=200_000,
        api_style=API_STYLE_ANTHROPIC,
    ),
    "openai_compatible": HostLlmPreset(
        "OpenAI 兼容 API",
        "https://api.openai.com/v1",
        "bearer",
        "gpt-4o-mini",
    ),
    "anthropic_compatible": HostLlmPreset(
        "Anthropic 兼容 API",
        "",
        "bearer",
        "",
        api_style=API_STYLE_ANTHROPIC,
    ),
    "local_openai": HostLlmPreset(
        # vLLM / Ollama / LM Studio 都是这个形状，且默认不校验 Key。
        "本地 OpenAI 兼容（vLLM/Ollama）",
        "http://127.0.0.1:8000/v1",
        "none",
        "",
    ),
    "custom": HostLlmPreset("自定义", "", "bearer", ""),
}

HOST_LLM_PRESET_ORDER: tuple[str, ...] = (
    "deepseek",
    "deepseek_flash",
    "anthropic",
    "openai",
    "bailian",
    "kimi_global",
    "kimi_cn",
    "openai_compatible",
    "anthropic_compatible",
    "local_openai",
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
            api_style=config.api_style,
            extra_headers=dict(config.extra_headers),
            extra_params=dict(config.extra_params),
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
        # 预设必须能改写 api_style：从 DeepSeek 切到 Anthropic 时如果沿用
        # 旧风格，请求会打到 /v1/messages 却带着 OpenAI 的体，必然 400。
        api_style=preset.api_style,
        extra_headers=dict(config.extra_headers),
        extra_params=dict(config.extra_params),
    )


def resolved_endpoint(config: HostLlmConfig) -> str:
    """这次配置实际会 POST 到哪个地址。给 GUI 和 config 子命令回显用。

    Base URL 填法五花八门（带不带 /v1、带不带完整路径），把归一后的结果
    显示出来，用户一眼就能看出 404 是不是自己粘多了一段路径。
    """
    try:
        return resolve_endpoint_url(config.base_url, llm_client.api_style_of(config))
    except RuntimeError as err:
        return f"(无法解析：{err})"


def read_json(path: Path, fallback: dict[str, Any]) -> dict[str, Any]:
    if not path.exists():
        return fallback
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return {**fallback, **data} if isinstance(data, dict) else fallback
    except json.JSONDecodeError:
        return fallback


def _str_dict(value: Any) -> dict[str, str]:
    if not isinstance(value, dict):
        return {}
    return {str(k): str(v) for k, v in value.items() if str(k).strip()}


def load_config(path: Path) -> HostLlmConfig:
    env_api_key = os.getenv("ESP_CLAW_HOST_LLM_API_KEY", "")
    data = read_json(path, asdict(HostLlmConfig()))
    if env_api_key:
        data["api_key"] = env_api_key

    # base_url 末尾的 # 是"锁定这个地址，别追加路径"的标记，不能被 rstrip 吃掉。
    raw_base_url = str(data.get("base_url") or "")
    base_url = raw_base_url if raw_base_url.endswith("#") else raw_base_url.rstrip("/")

    api_style = str(data.get("api_style") or "").strip()
    if api_style not in API_STYLES:
        # 旧配置没有这个字段，按地址推断一次，之后随配置一起落盘。
        api_style = llm_client.infer_api_style(base_url)

    extra_params = data.get("extra_params")
    config = HostLlmConfig(
        provider=str(data.get("provider") or "custom"),
        base_url=base_url,
        model=str(data.get("model") or ""),
        api_key=str(data.get("api_key") or ""),
        auth_type=str(data.get("auth_type") or "bearer"),
        max_tokens=int(data.get("max_tokens") or DEFAULT_MAX_TOKENS),
        system_prompt=str(data.get("system_prompt") or HostLlmConfig.system_prompt),
        api_style=api_style,
        extra_headers=_str_dict(data.get("extra_headers")),
        extra_params=dict(extra_params) if isinstance(extra_params, dict) else {},
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
        api_style=config.api_style,
        extra_headers=dict(config.extra_headers),
        extra_params=dict(config.extra_params),
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


#: 设备主动推送（不对应任何请求）时使用的固定 id，见 ble_host_bridge.c。
DEVICE_EVENT_REQUEST_ID = "job-event"


def _import_bleak() -> Any:
    """按需导入 bleak。

    plan / modules / 单元测试都不碰设备，没装 bleak 也应该能跑；
    只有真正要连 BLE 时才要求这个依赖。
    """
    try:
        import bleak
    except ImportError as err:
        raise RuntimeError(
            "需要 bleak 才能连接设备，请先安装：pip install -r requirements.txt"
        ) from err
    return bleak


class BleBridge:
    def __init__(
        self,
        device: Any | None,
        name_prefix: str,
        timeout: float,
        on_device_event: Callable[[dict[str, Any]], None] | None = None,
        on_audio_frame: Callable[[dict[str, Any]], None] | None = None,
    ) -> None:
        self.device = device
        self.name_prefix = name_prefix
        self.timeout = timeout
        self.client: BleakClient | None = None
        # 设备端 job 结束时会主动推一帧，这里收下来，避免被当成无主响应丢掉。
        self.on_device_event = on_device_event
        self.on_audio_frame = on_audio_frame
        self.last_device_event: dict[str, Any] | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._pending: dict[str, asyncio.Future[dict[str, Any]]] = {}
        self._response_chunks: dict[str, dict[str, Any]] = {}
        #: 串行化 _send。request() 本身按 req_id 分发，天生支持并发等待，
        #: 但分块写入不能交错：两个大请求同时发会把彼此的帧穿插到一起，
        #: 设备侧按 (id, seq) 重组时就会拼出半个 JSON。
        #: 惰性创建：BleBridge 可能在事件循环外被构造。
        self._write_lock: asyncio.Lock | None = None

    async def __aenter__(self) -> "BleBridge":
        await self.connect()
        return self

    async def __aexit__(self, exc_type: object, exc: object, tb: object) -> None:
        await self.disconnect()

    async def connect(self) -> None:
        target = self.device or await self._discover_device()
        self.client = _import_bleak().BleakClient(target)
        await self.client.connect(timeout=self.timeout)
        await self.client.start_notify(TX_UUID, self._on_notify)

    async def disconnect(self) -> None:
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        self._response_chunks.clear()

    async def _discover_device(self) -> str:
        scanner = _import_bleak().BleakScanner
        devices = await scanner.discover(timeout=self.timeout, service_uuids=[SERVICE_UUID])
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
            return
        if frame.get("type") == "workbuddy_audio_opus":
            if self.on_audio_frame:
                self.on_audio_frame(frame)
            return
        # 没有等待者：可能是设备主动推的 job 结果。
        if req_id == DEVICE_EVENT_REQUEST_ID or frame.get("type") == "job_event":
            self.last_device_event = frame
            if self.on_device_event:
                self.on_device_event(frame)

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

    async def send_nowait(self, command: dict[str, Any]) -> None:
        if not self.client or not self.client.is_connected:
            raise RuntimeError("BLE bridge 未连接")
        command = {**command, "id": str(command.get("id") or request_id("push"))}
        await self._send(command)

    async def _send(self, command: dict[str, Any]) -> None:
        assert self.client is not None
        text = json.dumps(command, ensure_ascii=False, separators=(",", ":"))

        if self._write_lock is None:
            self._write_lock = asyncio.Lock()

        # 整个写入过程持锁：分块之间有 await（sleep + GATT 写），不持锁的话
        # 另一个请求的帧会插进来，设备侧重组时拿到的就是两个请求的碎片。
        async with self._write_lock:
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


def http_timeout_for(max_tokens: int) -> float:
    """按输出预算放大读超时：token 越多，模型写得越久。"""
    scaled = HTTP_TIMEOUT_MIN_S + max(0, max_tokens) / 1000.0 * HTTP_TIMEOUT_PER_1K_TOKENS_S
    return min(HTTP_TIMEOUT_MAX_S, max(HTTP_TIMEOUT_MIN_S, scaled))


#: 从流式吐出来的 Lua 里认出"模型正在写什么"。按行匹配，先到先报。
STREAM_LANDMARKS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r'require\s*\(?\s*[\'"]([\w.]+)[\'"]'), "引入 {0} 模块"),
    (re.compile(r'^\s*local\s+function\s+([\w.:]+)'), "写 {0}() 函数"),
    (re.compile(r'^\s*function\s+([\w.:]+)'), "写 {0}() 函数"),
    (re.compile(r'^\s*while\s+.+\s+do\b'), "写主循环"),
    (re.compile(r'add_event_cb|_event_cb\s*\('), "接事件回调"),
    (re.compile(r'^\s*for\s+.+\s+do\b'), "写循环体"),
)


def make_progress_reporter(report: Callable[[str], None]) -> Callable[[str, bool], None]:
    """把流式增量翻译成"正在做什么"的状态行。

    字数是没有信息量的：用户看到"已生成 2400 字"并不知道进行到哪一步，
    也判断不了要不要再等。所以这里改成认代码里的路标——
    引入了哪个模块、在写哪个函数、有没有进主循环——外加阶段切换和心跳。

    回调约定：piece 为空字符串表示"没有新内容，但连接是活的"，
    用来在思维链很长、迟迟不写代码的那段时间里驱动心跳。
    """
    state = {"phase": "", "last": time.monotonic(), "start": time.monotonic()}
    pending = {"text": ""}
    seen: set[str] = set()

    def emit(message: str) -> None:
        state["last"] = time.monotonic()
        report(f"  {message}")

    def phase(name: str, message: str) -> None:
        """阶段只能往前走。

        SSE 每帧之间有空行，思维链和正文也可能交错出现，
        允许回退的话这两种情况都会让状态行反复横跳、把真正的路标冲掉。
        """
        if STREAM_PHASE_RANK[name] <= STREAM_PHASE_RANK[state["phase"]]:
            return
        state["phase"] = name
        emit(message)

    def heartbeat() -> None:
        if time.monotonic() - state["last"] < STREAM_HEARTBEAT_S:
            return
        elapsed = int(time.monotonic() - state["start"])
        hint = "仍在思考…" if state["phase"] == "reasoning" else "仍在写代码…"
        emit(f"{hint}（已 {elapsed}s）")

    def scan(text: str) -> None:
        """只扫完整的行：路标可能跨 chunk，留一截尾巴等下一块。"""
        pending["text"] += text
        *lines, pending["text"] = pending["text"].split("\n")
        for line in lines:
            for pattern, template in STREAM_LANDMARKS:
                found = pattern.search(line)
                if not found:
                    continue
                message = template.format(*found.groups())
                if message not in seen:
                    seen.add(message)
                    emit(message)
                break

    def on_progress(piece: str, is_reasoning: bool) -> None:
        if not piece:
            phase("waiting", "已连接，等待模型响应…")
            heartbeat()
            return
        if is_reasoning:
            phase("reasoning", "正在分析需求…")
            heartbeat()
            return
        phase("content", "开始输出脚本…")
        scan(piece)
        heartbeat()

    return on_progress


def _truncated_error(budget: int, usage: llm_client.Usage, *, only_reasoning: bool) -> RuntimeError:
    """输出被 max_tokens 截断时的报错。

    这必须是一次失败而不是"凑合能用"：截断的回复常常缺收尾 fence，或只剩
    程序前半段——语法上能通过、上传后 1 秒就退（屏幕闪一下回主界面）。
    """
    written = usage.output_tokens if usage.output_tokens is not None else "?"
    if only_reasoning:
        return RuntimeError(
            "LLM 只产出了思维链就被 max_tokens 截断，没有正文。"
            f"（completion_tokens={written}，本次 max_tokens={budget}）请调大 max_tokens。"
        )
    return RuntimeError(
        "LLM 输出被 max_tokens 截断，脚本不完整，已丢弃。"
        f"（completion_tokens={written}，本次 max_tokens={budget}）请在设置里调大 max_tokens。"
    )


def call_llm(
    config: HostLlmConfig,
    messages: list[dict[str, str]],
    *,
    max_tokens: int | None = None,
    on_progress: Callable[[str, bool], None] | None = None,
    on_note: Callable[[str], None] | None = None,
    cancel: Cancellation | None = None,
) -> str:
    """跑一次生成，返回正文。

    HTTP、SSE、参数降级、鉴权全在 llm_client 里，这里只负责三件本项目特有的事：
    1. 决定这次的输出预算和两个超时；
    2. 截断要当场判失败（半个脚本上传后秒退，比报错更难查）；
    3. thinking 模型把正文写进 reasoning 时捞回来。
    """
    budget = max_tokens or config.max_tokens

    result = llm_client.generate(
        config,
        messages,
        max_tokens=budget,
        # 流式下超时的语义是"多久没有新字节"，不该跟着预算放大：
        # 模型写得慢不会触发，网络真断了才会。
        stream_read_timeout=HTTP_STREAM_STALL_TIMEOUT_S,
        # 非流式没有增量输出，只能按总耗时给，且要随预算放大。
        total_read_timeout=http_timeout_for(budget),
        on_progress=on_progress,
        on_note=on_note,
        cancel=cancel,
    )

    if on_note and result.degraded:
        on_note("为兼容该服务做了调整：" + "；".join(result.degraded))

    if result.text.strip():
        if result.finish == "length":
            raise _truncated_error(budget, result.usage, only_reasoning=False)
        return result.text

    # thinking 类模型（kimi-k2.6、claude thinking 等）可能把正文写进 reasoning，
    # content 全空。之前直接报"LLM 返回为空"，其实正文就在这里。
    if "```" in result.reasoning:
        return result.reasoning
    if result.finish == "length":
        raise _truncated_error(budget, result.usage, only_reasoning=True)
    if result.finish == "content_filter":
        raise RuntimeError("LLM 拒绝了这次请求（内容过滤）。请调整提示词后重试。")

    usage = result.usage.describe()
    raise RuntimeError(
        f"LLM 返回为空（finish={result.finish or '?'}"
        + (f"，用量：{usage}" if usage else "")
        + "）"
    )


#: 旧名字，保留给已有调用点和外部脚本。
call_openai_compatible = call_llm


LUA_FENCE_RE = re.compile(r"```[ \t]*(?:lua)?[ \t]*\r?\n([\s\S]*?)```", re.IGNORECASE)
LUA_OPEN_FENCE_RE = re.compile(r"```[ \t]*(?:lua)?[ \t]*\r?\n([\s\S]*)$", re.IGNORECASE)


def _lua_block_rank(block: str) -> tuple[int, int]:
    """越像“完整程序”排越前：有 require / 主循环的加权，其次比长度。"""
    weight = 0
    if re.search(r"\brequire\s*\(", block):
        weight += 2
    if re.search(r"^\s*(?:while|for)\b", block, flags=re.MULTILINE):
        weight += 1
    return weight, len(block)


def extract_lua(text: str) -> str:
    """从 LLM 回复里取出 Lua 源码。

    刻意不取“第一个”代码块：模型常先给一小段示意片段、再给完整实现，
    取第一个会把片段当成整个程序上传（实测得到 index a nil value (global 'state')）。
    也要处理被 max_tokens 截断、收尾 fence 缺失的回复，否则整段原文连 ``` 一起
    被当成 Lua 上传（实测报 unexpected symbol near '`'）。
    """
    blocks = LUA_FENCE_RE.findall(text or "")
    if not blocks:
        # 没有成对 fence，说明回复被截断：取开启 fence 之后的全部内容。
        open_match = LUA_OPEN_FENCE_RE.search(text or "")
        blocks = [open_match.group(1)] if open_match else [text or ""]
    best = max(blocks, key=_lua_block_rank)
    # 兜底：无论走哪条路径，都不允许 fence 残留流到设备。
    kept = [ln for ln in best.splitlines() if not ln.lstrip().startswith("```")]
    return "\n".join(kept).strip()


@dataclass(frozen=True)
class ModulePlan:
    """第一阶段路由结果：这次任务要用哪些设备模块。"""

    modules: list[str]
    task_kind: str = ""
    notes: str = ""
    source: str = "keywords"
    docs: list[str] = field(default_factory=list)

    def describe(self) -> str:
        modules = ", ".join(self.modules) or "(无)"
        parts = [f"模块选择（{self.source}）：{modules}"]
        if self.task_kind:
            parts.append(f"任务类型：{self.task_kind}")
        if self.notes:
            parts.append(f"路由说明：{self.notes}")
        if self.docs:
            parts.append("载入 README：" + ", ".join(self.docs))
        return "\n".join(parts)


ROUTER_SYSTEM_PROMPT = (
    "你是 ESP-Claw 设备端 Lua 模块路由器。"
    "根据用户需求，从下面的模块清单里挑出实现该需求真正需要的模块。"
    "只输出一个 JSON 对象，不要 markdown 代码块，不要解释文字，格式为："
    '{"modules":["display","delay"],"task_kind":"简短任务类型","notes":"一句话理由"}。'
    "规则："
    "1. modules 只能取清单里出现过的名字，不要虚构；"
    "2. 只选必要的模块，一般 2-6 个；不确定要不要的宁可不选；"
    "3. 默认选 lvgl：游戏、动画、按钮、列表、滑块、触摸交互都优先用 lvgl；"
    "   只有用户明确要求逐像素自绘，或画面无法用 lvgl 对象表达时才选 display；"
    "   display 和 lvgl 不要同时选，除非用户明确要求两种界面；"
    "4. 需要屏幕时必须带 board_manager；需要循环、节拍或保持画面时带 delay；"
    "5. 触屏操作选 lcd_touch，实体按键选 button。"
)


def plan_modules(config: HostLlmConfig, prompt: str) -> ModulePlan:
    """第一阶段：先让 LLM 判断要用哪些设备能力，失败则退回关键词匹配。"""
    keyword_modules = catalog.select_by_keywords(prompt)
    messages = [
        {"role": "system", "content": ROUTER_SYSTEM_PROMPT},
        {"role": "system", "content": f"可用模块清单：\n{catalog.catalog_overview()}"},
        {"role": "user", "content": prompt},
    ]

    try:
        raw = call_llm(config, messages, max_tokens=400)
        payload = extract_json_object(raw)
        modules = catalog.normalize_requires(payload.get("modules"))
        if not modules:
            raise ValueError("路由结果没有可用模块")
        merged = catalog.expand_with_implied(modules)
        for name in keyword_modules:
            if name not in merged:
                merged.append(name)
        return ModulePlan(
            modules=merged,
            task_kind=str(payload.get("task_kind") or ""),
            notes=str(payload.get("notes") or ""),
            source="llm",
        )
    except Exception as err:
        return ModulePlan(
            modules=keyword_modules,
            source="keywords",
            notes=f"LLM 路由不可用，已退回关键词匹配：{err}",
        )


def extract_json_object(text: str) -> dict[str, Any]:
    """从 LLM 回复里取出第一个 JSON 对象，容忍 ```json 包裹和前后闲聊。"""
    stripped = text.strip()
    fence = re.search(r"```(?:json)?\s*([\s\S]*?)```", stripped, flags=re.IGNORECASE)
    if fence:
        stripped = fence.group(1).strip()

    start = stripped.find("{")
    end = stripped.rfind("}")
    if start < 0 or end <= start:
        raise ValueError("回复里没有 JSON 对象")
    payload = json.loads(stripped[start:end + 1])
    if not isinstance(payload, dict):
        raise ValueError("JSON 顶层不是对象")
    return payload


@dataclass(frozen=True)
class JobOutcome:
    """一次设备端执行的结果，从 job_status 的 key=value 文本解析而来。"""

    job_id: str
    status: str
    summary: str = ""
    log: str = ""
    runtime_s: int = 0
    submit_text: str = ""
    log_truncated: bool = False
    #: 本次任务是不是本该长跑（游戏、交互界面）。判定成败的方向完全相反，见 ok。
    expects_long_run: bool = False

    @property
    def ok(self) -> bool:
        if self.has_runtime_error:
            return False
        if not self.expects_long_run:
            return self.status == "done"
        # 界面/游戏脚本必须一直跑，成败方向和一次性脚本相反：
        # - running：不限时执行下的正常终局。轮询到点脚本还在跑，画面就还在，
        #   这是游戏类任务最主要的成功信号（设备不会再报 timeout）。
        # - timeout：旧固件/显式限时下的成功信号，主循环转到了被掐掉那一刻。
        # - done 且秒退：画面闪一下就回主界面，失败。
        if self.status in {"running", "queued"}:
            return self.runtime_s >= LONG_RUN_MIN_S
        if self.status == "timeout":
            return True
        if self.status == "done":
            return self.runtime_s >= LONG_RUN_MIN_S
        return False

    @property
    def flickered(self) -> bool:
        """界面脚本秒退：屏幕闪一下就回到主界面。"""
        return (
            self.expects_long_run
            and self.status == "done"
            and self.runtime_s < LONG_RUN_MIN_S
        )

    @property
    def has_diagnostics(self) -> bool:
        """设备是否真的给出了可用于修脚本的信息。"""
        if self.flickered:
            # 设备端不会为"跑完就退"报错，诊断由上位机合成，见 FLICKER_DIAGNOSIS。
            return True
        return bool(self.meaningful(self.summary) or self.meaningful(self.log))

    @staticmethod
    def meaningful(text: str) -> str:
        stripped = (text or "").strip()
        return "" if stripped in {"", "(empty)", "(none)", "(unnamed)"} else stripped

    @property
    def has_runtime_error(self) -> bool:
        """Some generated scripts catch their own frame error and then exit done."""
        text = f"{self.meaningful(self.summary)}\n{self.meaningful(self.log)}"
        return bool(re.search(r"(?:^|\n)\s*(?:ERROR|\[ERR\]|Lua error:)|attempt to|bad argument", text))

    @property
    def retryable(self) -> bool:
        """只有真正的 Lua 报错值得再花一次 LLM 调用。

        timeout 通常意味着脚本是长跑的（游戏主循环、交互界面），本身就是预期行为；
        stopped 是有人主动停的。这两种重试只会白烧 token。
        """
        if self.status == "failed":
            return True
        if self.has_runtime_error:
            return True
        if self.flickered:
            # 缺主循环是模型自己能改的，且我们能给出明确诊断，值得再要一轮。
            return True
        if self.status == "timeout":
            if self.expects_long_run:
                return False
            # 跑起来又有日志输出，说明脚本是在正常长跑，不是写错了。
            return not self.has_diagnostics and self.runtime_s <= 1
        return False

    def error_signature(self) -> str:
        """用于判断两轮是不是同一个错误：取日志里第一条 Lua 报错。"""
        log = self.meaningful(self.log)
        summary = self.meaningful(self.summary)
        text = f"{log}\n{summary}".strip()
        if not text:
            return f"{self.status}:no-diagnostics"
        match = re.search(r"[^\n]*(?:error|Error|ERROR|attempt to|bad argument)[^\n]*", text)
        line = match.group(0) if match else text.strip().splitlines()[-1]
        # 去掉脚本路径里的 job id，避免同一个错误因为路径不同被当成两个。
        return f"{self.status}:{re.sub(r'[0-9a-f]{6,}', 'ID', line.strip())[:180]}"

    def describe(self) -> str:
        parts = [f"status={self.status}", f"job_id={self.job_id or '(未知)'}"]
        if self.runtime_s:
            parts.append(f"runtime_s={self.runtime_s}")
        head = " ".join(parts)
        if self.flickered:
            head += "\n" + FLICKER_DIAGNOSIS.format(runtime=self.runtime_s)
        summary = self.meaningful(self.summary)
        log = self.meaningful(self.log)
        if summary:
            head += f"\nsummary: {summary}"
        if log:
            head += f"\n设备日志：\n{log[-1800:]}"
        if not summary and not log:
            head += "\n设备没有返回 summary 或日志。"
        return head


@dataclass
class Attempt:
    """一轮生成 + 执行的完整记录。"""

    index: int
    script: str
    raw: str = ""
    outcome: JobOutcome | None = None
    error: str = ""
    stopped_because: str = ""

    @property
    def ok(self) -> bool:
        return bool(self.outcome and self.outcome.ok)


def parse_job_status(text: str) -> JobOutcome:
    """解析 cap_lua 的 job_status 纯文本输出。

    格式是 key=value 逐行，recent_log 在最后且值里可能带换行，所以要单独切出来。
    """
    log = ""
    head = text or ""
    for marker in ("\nrecent_log=", "\nlog="):
        position = head.find(marker)
        if position >= 0:
            log = head[position + len(marker):]
            head = head[:position]
            break

    fields: dict[str, str] = {}
    for line in head.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            fields[key.strip()] = value.strip()

    try:
        runtime_s = int(fields.get("runtime_s") or 0)
    except ValueError:
        runtime_s = 0

    return JobOutcome(
        job_id=fields.get("job_id", ""),
        status=fields.get("status", "unknown"),
        summary=fields.get("summary", ""),
        log=log,
        runtime_s=runtime_s,
        log_truncated=fields.get("log_truncated", "").lower() == "true",
    )


def extract_job_id(text: str) -> str:
    """从 run_lua 的返回文本里取 job id（设备端返回的是人类可读文本，不是 JSON）。"""
    for pattern in (r"job_id=([0-9a-zA-Z]+)", r"Lua job ([0-9a-zA-Z]+)"):
        match = re.search(pattern, text or "")
        if match:
            return match.group(1)
    return ""


def normalize_script(script: str) -> str:
    """用于比较两轮脚本是否实质相同：忽略空白与注释差异。"""
    lines = []
    for line in (script or "").splitlines():
        line = re.sub(r"--.*$", "", line).strip()
        if line:
            lines.append(re.sub(r"\s+", " ", line))
    return "\n".join(lines)


#: 常驻脚本的两种合法形态，见 lua_module_lvgl/README.md:13。
_LVGL_RESIDENT_RE = re.compile(r"lvgl\.run\s*\(|while\s+.*\s+do[\s\S]*?lvgl\.process_events")
#: 建了屏幕/控件就说明这脚本要出画面，画面必须撑住才有意义。
_UI_SCRIPT_RE = re.compile(r"lvgl\.(?:create_screen|init)\s*\(|display\.init\s*\(")


def script_expects_long_run(script: str) -> bool:
    """从脚本本身判断它该不该常驻。

    prompt 不总是可靠（手动上传时根本没有 prompt），但两件事是确定的：
    - 建了 LVGL 屏幕：Lua 一退出画面就销毁，done+秒退必然是失败；
    - 写了驻留主循环：脚本自己声明了要一直跑，不该给有限的执行窗口。
    只认第一条会漏掉"复用已初始化的屏幕、只写主循环"的脚本。
    """
    body = script or ""
    return bool(_UI_SCRIPT_RE.search(body) or _LVGL_RESIDENT_RE.search(body))


def validate_generated_lua(script: str) -> None:
    errors: list[str] = []
    body = script.strip()

    # 上一轮的两次 status=failed 都是"上传了不完整的东西"，在这里拦掉，省一次设备往返。
    if "```" in body or re.search(r"^\s*`", body, re.MULTILINE):
        errors.append("脚本里残留 Markdown 代码围栏或反引号，说明模型输出被截断，需重新生成完整脚本")
    if "require(" not in body and "require " not in body:
        errors.append("脚本没有任何 require()，这是代码片段而不是完整脚本，必须输出可独立运行的整份脚本")

    # 秒退：只建对象不驻留，Lua 一退出 LVGL 对象全销毁，屏幕闪一下就回主界面。
    # 判据取自脚本本身而不是 task_kind：手动上传时没有 plan，但"建了屏幕"是确定的。
    if script_expects_long_run(body) and "lvgl" in body and not _LVGL_RESIDENT_RE.search(body):
        errors.append(
            "脚本建了 LVGL 屏幕却没有常驻主循环，会立刻退出导致屏幕闪一下就回主界面："
            "必须以 `lvgl.run()` 或 `while true do ... lvgl.process_events(16) ... end` 结尾"
        )

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
    if re.search(r"pcall\s*\(\s*(?:game_loop|main_loop)\s*\)", script):
        errors.append(
            "不要用 pcall(game_loop/main_loop) 吞掉主循环错误后正常退出；"
            "调试版应 print 后 error(err)，否则设备会 status=done 但实际已经崩了"
        )
    if re.search(r"pcall\s*\(\s*\w+\.new_output\s*,\s*\w+\s*,\s*\{", script):
        errors.append(
            "audio.new_output 是普通函数，不要把 audio 模块作为 self 参数传入；"
            "使用 audio.new_output({ codec, rate, channels, bits, volume = 70 })"
        )
    if (
        "lvgl.font_load" in script
        and "storage.get_root_dir" in script
        and "fonts/" in script
        and "NotoSansSC-Regular-sub.ttf" in script
    ):
        errors.append(
            "不要从 storage.get_root_dir()/fonts 手动加载 NotoSansSC-Regular-sub.ttf；"
            "DATA 根目录可能没有该字体。普通标签使用 lvgl.init({font_size=...}) 的默认字体，"
            "只有明确安装字体文件时才用 pcall(lvgl.font_load, ...)"
        )
    if errors:
        raise RuntimeError("生成的 Lua 未通过上传前校验：" + "；".join(dict.fromkeys(errors)))


def plan_for(
    config: HostLlmConfig,
    prompt: str,
    device_info: str = "",
    *,
    on_note: Callable[[str], None] | None = None,
    cancel: Cancellation | None = None,
) -> TaskPlan:
    """规划入口：把 HTTP 客户端注入 task_planner，产出带步骤和风险的完整计划。

    必须走这里而不是 plan_modules()：上下文引擎的 plan provider 只认 TaskPlan，
    喂 ModulePlan 会让「实现计划」段被整段静默丢掉。
    """

    def provider(messages: list[dict[str, str]], max_tokens: int) -> str:
        # 规划阶段也要能被取消：它同样是一次完整的 LLM 往返，
        # 之前点"停止"只能等它自己跑完。
        return call_llm(
            config, messages, max_tokens=max_tokens, on_note=on_note, cancel=cancel
        )

    return plan_task(prompt, provider, device_info=device_info)


def build_messages(
    prompt: str,
    device_info: str,
    history: list[dict[str, Any]],
    system_prompt: str | None = None,
    plan: TaskPlan | ModulePlan | None = None,
    attempts: list["Attempt"] | None = None,
) -> list[dict[str, str]]:
    """用上下文引擎组装消息，返回可直接送进 OpenAI 接口的 messages 列表。"""
    return build_context(
        prompt, device_info, history, system_prompt, plan, attempts
    ).messages


def build_context(
    prompt: str,
    device_info: str,
    history: list[dict] | None = None,
    system_prompt: str | None = None,
    plan: TaskPlan | ModulePlan | None = None,
    attempts: list["Attempt"] | None = None,
) -> BuiltContext:
    """同 build_messages，但额外返回每段的装配轨迹，用于诊断提示词组装结果。

    分段、排序、预算裁剪、修复回灌全部由 provider 决定，这里只负责传参。
    """
    request = ContextRequest(
        prompt=prompt,
        device_info=device_info,
        history=history or [],
        plan=plan,
        attempts=attempts or [],
    )
    builder = host_context.build_default_builder(system_prompt=system_prompt or "")
    return builder.build(request)

def repair_messages(attempts: list["Attempt"]) -> list[dict[str, str]]:
    """把前几轮真实的设备执行结果回灌给 LLM，用于迭代修脚本。"""
    out: list[dict[str, str]] = []
    for idx, attempt in enumerate(attempts, 1):
        out.append({"role": "assistant", "content": f"```lua\n{attempt.script}\n```"})

        if attempt.error:
            detail = f"## 第 {idx} 轮执行失败（上位机本地校验）\n\n**错误信息**：{attempt.error}\n"
        elif attempt.outcome:
            outcome = attempt.outcome
            detail = f"## 第 {idx} 轮执行失败（设备端运行）\n\n"
            detail += f"**状态**：{outcome.status}\n"
            diagnostic_blob = f"{outcome.summary}\n{outcome.log}"
            if outcome.summary:
                detail += f"**错误摘要**：{outcome.summary}\n"

            # 分析常见错误模式并给出针对性提示
            if "attempt to call a nil value" in diagnostic_blob:
                # 提取函数名
                match = re.search(r"attempt to call a nil value \(field '(\w+)'\)", diagnostic_blob)
                if match:
                    func_name = match.group(1)
                    detail += f"\n**问题分析**：函数 `{func_name}` 不存在。请回到 README 确认该模块是否有这个函数，或者是否需要不同的初始化方式。\n"

            elif "bad argument" in diagnostic_blob and "expected" in diagnostic_blob:
                detail += "\n**问题分析**：函数参数类型或个数不正确。请检查 README 中该函数的完整签名。\n"

            elif "handle expected" in diagnostic_blob:
                detail += "\n**问题分析**：缺少必需的句柄参数。某些函数（如 lcd_touch.poll）必须传入通过 board_manager 获取的句柄。\n"

            elif "lvgl failed to load font" in diagnostic_blob:
                detail += (
                    "\n**问题分析**：脚本手动加载了设备 DATA 根目录下不存在的字体。"
                    "普通标签不要调用 `lvgl.font_load(storage.get_root_dir() .. \"/fonts/...\")`；"
                    "使用 `lvgl.init({ font_size = ... })` 的默认字体即可。"
                    "如果必须加载自定义字体，必须先确认文件存在，并用 `pcall(lvgl.font_load, ...)` 失败降级。\n"
                )

            if outcome.log:
                detail += f"\n**完整日志**：\n```\n{outcome.log[:500]}\n```\n"
        else:
            detail = f"## 第 {idx} 轮：未拿到执行结果\n"

        out.append({
            "role": "user",
            "content": (
                f"{detail}\n"
                "## 修复要求\n"
                "1. **只修改导致错误的部分**，不要重写整个脚本\n"
                "2. **必须回到 README 确认函数签名**，不要凭记忆或猜测\n"
                "3. **不要重复提交相同的代码**\n"
                "4. 如果某个函数在 README 中找不到，说明它不存在，必须用其他方式实现\n"
                "5. 仍然只返回一个 lua 代码块\n"
            ),
        })
    return out


async def upload_lua(bridge: BleBridge, script: str, timeout: float) -> str:
    """把脚本写到设备 /scripts/host 下，返回设备上的路径。"""
    put = await bridge.request(
        {"id": request_id("lua"), "op": "put_lua", "source": script},
        timeout=timeout,
    )
    if not put.get("ok"):
        raise RuntimeError(f"put_lua 失败：{put.get('error') or put}")
    path = str(put.get("data") or "")
    if not path:
        raise RuntimeError("put_lua 没有返回脚本路径")
    return path


async def run_lua_and_collect(
    bridge: BleBridge,
    script: str,
    *,
    timeout: float = DEFAULT_TIMEOUT,
    lua_timeout_ms: int | None = None,
    poll_timeout: float = JOB_POLL_TIMEOUT,
    poll_interval: float = JOB_POLL_INTERVAL,
    on_event: Callable[[str], None] | None = None,
    expects_long_run: bool | None = None,
    stop_existing: bool = True,
) -> JobOutcome:
    """上传、执行，然后轮询 job_status 直到拿到终态或超出耐心。

    设备端 run_lua 是异步的，只返回 job id；真正的执行结果要靠 job_status 取回。

    expects_long_run 不传时从脚本自身推断，这样手动上传（GUI 的"上传并执行"、
    CLI 的 run-lua）也能正确判定：建了 LVGL 屏幕却秒退 = 画面闪一下就没了 = 失败。
    """
    if expects_long_run is None:
        expects_long_run = script_expects_long_run(script)
    effective_timeout_ms = resolve_lua_timeout_ms(lua_timeout_ms, expects_long_run)

    def report(message: str) -> None:
        if on_event:
            on_event(message)

    if stop_existing:
        try:
            report("执行前先停止设备上已有 Lua job，等待 LVGL 资源释放…")
            stop_resp, previous = await stop_job_and_wait(
                bridge,
                job_id=None,
                timeout=min(timeout, 5),
                wait_after_stop=STOP_EXISTING_WAIT_AFTER_STOP_S,
            )
            stop_text = str(stop_resp.get("data") or stop_resp.get("error") or "")
            if "still unwinding" in stop_text:
                raise RuntimeError(stop_text)
            if previous.status and previous.status != "unknown":
                report(f"旧 job 状态：{previous.status}")
            if previous.status and previous.status not in TERMINAL_JOB_STATUS and previous.status != "unknown":
                raise RuntimeError(f"旧 Lua job 未停止，当前状态：{previous.status}")
            if not stop_resp.get("ok") and previous.status not in TERMINAL_JOB_STATUS:
                idle_or_missing = any(
                    marker in stop_text
                    for marker in ("job_id_required", "ESP_ERR_NOT_FOUND", "not found")
                )
                if not idle_or_missing:
                    raise RuntimeError(stop_text or stop_resp)
            if previous.status in TERMINAL_JOB_STATUS:
                await asyncio.sleep(STOP_EXISTING_SETTLE_S)
        except Exception as err:
            # 旧 job 没确认停止时不要提交新脚本，否则 LVGL/触摸资源占用会被误判成新代码失败。
            raise RuntimeError(f"停止旧 Lua job 未确认，已取消本轮执行：{err}") from err

    path = await upload_lua(bridge, script, timeout)
    report(f"已上传到设备：{path}")
    if effective_timeout_ms == LUA_TIMEOUT_UNTIL_CANCELLED:
        report("执行窗口：不限时（常驻脚本，跑到被停止为止）")
    else:
        report(f"执行窗口：{effective_timeout_ms / 1000:.0f}s 后由设备终止")

    run = await bridge.request(
        {
            "id": request_id("run"),
            "op": "run_lua",
            "path": path,
            "timeout_ms": effective_timeout_ms,
        },
        timeout=timeout,
    )
    submit_text = str(run.get("data") or run.get("error") or "")
    if not run.get("ok"):
        raise RuntimeError(f"run_lua 失败：{submit_text or run}")

    job_id = extract_job_id(submit_text)
    report(f"已提交执行：job_id={job_id or '(未解析到)'}")

    outcome = await poll_job_status(
        bridge,
        job_id,
        timeout=timeout,
        poll_timeout=poll_timeout,
        poll_interval=poll_interval,
        on_event=on_event,
    )

    log = outcome.log
    diagnostic_text = f"{outcome.summary}\n{log}"
    needs_full_log = (
        not outcome.ok
        and (
            outcome.log_truncated
            or not JobOutcome.meaningful(log)
            or not re.search(r"(?:ERROR|Error|error|attempt to|bad argument)", diagnostic_text)
        )
    )
    if needs_full_log:
        # job_status 只带一段有限的日志尾巴，报错的 traceback 可能被截断。
        await asyncio.sleep(0.8)
        full = await fetch_job_log(bridge, outcome.job_id or job_id, timeout=timeout)
        if full:
            report("已拉取完整日志用于诊断")
            log = full

    return JobOutcome(
        job_id=outcome.job_id or job_id,
        status=outcome.status,
        summary=outcome.summary,
        log=log,
        runtime_s=outcome.runtime_s,
        submit_text=submit_text,
        log_truncated=outcome.log_truncated,
        expects_long_run=expects_long_run,
    )


async def fetch_job_log(
    bridge: BleBridge,
    job_id: str,
    *,
    timeout: float = DEFAULT_TIMEOUT,
    max_bytes: int = 3000,
) -> str:
    """用 job_tail 从头拉日志。设备端不支持该 op 的旧固件会返回错误，此时返回空串。"""
    if not job_id:
        return ""
    try:
        response = await bridge.request(
            {
                "id": request_id("tail"),
                "op": "job_tail",
                "job_id": job_id,
                "since_seq": 0,
                "max_bytes": max_bytes,
            },
            timeout=timeout,
        )
    except Exception:
        return ""
    if not response.get("ok"):
        return ""
    text = str(response.get("data") or "")
    marker = "\nlog="
    position = text.find(marker)
    return text[position + len(marker):] if position >= 0 else ""


async def poll_job_status(
    bridge: BleBridge,
    job_id: str,
    *,
    timeout: float = DEFAULT_TIMEOUT,
    poll_timeout: float = JOB_POLL_TIMEOUT,
    poll_interval: float = JOB_POLL_INTERVAL,
    on_event: Callable[[str], None] | None = None,
) -> JobOutcome:
    """轮询到终态为止；到点还在 running 就按 running 返回（长跑脚本是正常的）。"""
    started = time.monotonic()
    deadline = started + poll_timeout
    last = JobOutcome(job_id=job_id, status="unknown")
    last_status = ""
    last_beat = started

    while True:
        command: dict[str, Any] = {"id": request_id("job"), "op": "job_status"}
        if job_id:
            command["job_id"] = job_id
        status = await bridge.request(command, timeout=timeout)
        text = str(status.get("data") or status.get("error") or "")
        if status.get("ok"):
            last = parse_job_status(text)
            if last.status != last_status and on_event:
                on_event(f"job 状态：{last.status}")
                last_beat = time.monotonic()
            last_status = last.status
            if last.status in TERMINAL_JOB_STATUS:
                return last
            # 长跑脚本会一直停在 running：状态不变也要间歇报一声，
            # 否则等满 poll_timeout 的这段时间界面上什么都没有。
            now = time.monotonic()
            if on_event and now - last_beat >= JOB_HEARTBEAT_S:
                on_event(f"脚本仍在设备上运行（已 {now - started:.0f}s）")
                last_beat = now
        elif on_event:
            on_event(f"job_status 查询失败：{text}")

        if time.monotonic() >= deadline:
            return last
        await asyncio.sleep(poll_interval)


async def stop_job_and_wait(
    bridge: BleBridge,
    job_id: str | None = None,
    *,
    timeout: float = DEFAULT_TIMEOUT,
    wait_after_stop: float = 2.0,
) -> tuple[dict[str, Any], JobOutcome]:
    """发送停止请求并短暂等待确认，不做长时间轮询。

    Returns:
        (stop_response, final_status)
    """
    # 发送停止请求
    stop_resp = await bridge.request(
        {
            "id": request_id("stop"),
            "op": "job_stop",
            **({"job_id": job_id} if job_id else {}),
        },
        timeout=timeout,
    )

    # 等待一小段时间让设备处理
    await asyncio.sleep(0.5)

    # 轮询几次确认状态（最多 wait_after_stop 秒）
    started = time.monotonic()
    last_outcome = JobOutcome(job_id=job_id or "", status="unknown")

    while time.monotonic() - started < wait_after_stop:
        status_resp = await bridge.request(
            {
                "id": request_id("job"),
                "op": "job_status",
                **({"job_id": job_id} if job_id else {}),
            },
            timeout=timeout,
        )
        text = str(status_resp.get("data") or status_resp.get("error") or "")
        if status_resp.get("ok"):
            last_outcome = parse_job_status(text)
            # 如果已经到达终态，立即返回
            if last_outcome.status in TERMINAL_JOB_STATUS:
                break
        await asyncio.sleep(0.3)

    return stop_resp, last_outcome



async def iterate_lua(
    bridge: BleBridge,
    config: HostLlmConfig,
    prompt: str,
    device_info: str,
    history: list[dict[str, Any]],
    *,
    plan: TaskPlan | None = None,
    max_attempts: int = 1,
    timeout: float = DEFAULT_TIMEOUT,
    lua_timeout_ms: int | None = None,
    poll_timeout: float = JOB_POLL_TIMEOUT,
    on_event: Callable[[str], None] | None = None,
    cancel: Cancellation | None = None,
    web_preview_check: bool = False,
) -> tuple[list[Attempt], TaskPlan]:
    """生成 → 执行 → 读回真实结果 → 修脚本，直到成功或判定继续重试没意义。

    这里刻意不是"失败就重来 N 次"：每一轮都要先确认再花一次 LLM 调用是有依据的，
    否则重复烧 token 只会产出几乎一样的脚本。
    """
    def report(message: str) -> None:
        if on_event:
            on_event(message)

    def check_cancelled() -> None:
        """在每个可能很久的步骤之前检查一次。

        取消不该只在 HTTP 层生效：一轮迭代里 LLM 之外还有上传、轮询设备，
        中间任何一处都可能等好几十秒。
        """
        if cancel is not None and cancel.is_set():
            raise Cancelled("已取消")

    if plan is None:
        report("正在规划实现方案…")
        plan = await asyncio.to_thread(
            plan_for, config, prompt, device_info, on_note=report, cancel=cancel
        )
        report(plan.describe())

    attempts: list[Attempt] = []
    seen_scripts: set[str] = set()
    last_signature = ""
    # 游戏/界面类任务的成败方向和一次性脚本相反：done 且秒退是失败，timeout 才是成功。
    long_run = wants_long_run(prompt, getattr(plan, "task_kind", ""))
    if long_run:
        report(f"判定为常驻类任务：done 且运行 <{LONG_RUN_MIN_S}s 视为失败（画面闪一下就退）")

    # 完整游戏脚本 200+ 行，4096 token 装不下；截断后上传的是半个程序，
    # 语法能过但秒退。常驻类任务直接把预算提到够写完一份完整脚本。
    gen_budget = max(config.max_tokens, LONG_RUN_MIN_TOKENS) if long_run else config.max_tokens
    if gen_budget > config.max_tokens:
        report(f"常驻类任务生成预算提到 max_tokens={gen_budget}（配置值 {config.max_tokens} 装不下完整脚本）")

    for index in range(1, max(1, max_attempts) + 1):
        built = build_context(prompt, device_info, history, config.system_prompt, plan, attempts)
        messages = built.messages
        if index == 1:
            if plan.docs:
                report("载入 README：" + ", ".join(plan.docs))
            report(built.describe())
        report(f"第 {index}/{max_attempts} 轮：正在生成 Lua…")
        check_cancelled()
        raw = await asyncio.to_thread(
            call_llm,
            config,
            messages,
            max_tokens=gen_budget,
            on_progress=make_progress_reporter(report),
            on_note=report,
            cancel=cancel,
        )
        script = extract_lua(raw)
        attempt = Attempt(index=index, script=script, raw=raw)

        fingerprint = normalize_script(script)
        if fingerprint and fingerprint in seen_scripts:
            attempt.stopped_because = "LLM 重新生成了与上一轮实质相同的脚本，继续重试不会有新结果"
            attempts.append(attempt)
            report(attempt.stopped_because)
            break
        seen_scripts.add(fingerprint)

        try:
            validate_generated_lua(script)
        except RuntimeError as err:
            attempt.error = str(err)
            attempts.append(attempt)
            report(f"本地校验未通过：{err}")
            if index >= max_attempts:
                break
            continue

        if web_preview_check:
            report("正在运行真实 LVGL Web 预览自动检查…")
            check_cancelled()
            preview = await asyncio.to_thread(
                run_web_lvgl_check,
                script,
                name=f"attempt-{index}.lua",
                expects_long_run=long_run,
                min_runtime_s=float(LONG_RUN_MIN_S),
                timeout_s=max(float(LONG_RUN_MIN_S) + 1.0, 6.0),
            )
            report(preview.describe())
            if not preview.skipped and not preview.ok:
                attempt.outcome = JobOutcome(
                    job_id=f"web-preview-{index}",
                    status="failed",
                    summary="Web 预览失败：" + preview.reason,
                    log=preview.log,
                    runtime_s=int(preview.runtime_s),
                    expects_long_run=long_run,
                )
                attempts.append(attempt)
                signature = attempt.outcome.error_signature()
                if signature == last_signature:
                    attempt.stopped_because = f"连续两轮同一个 Web 预览错误（{signature}），LLM 没有收敛，不再重试"
                    report(attempt.stopped_because)
                    break
                last_signature = signature
                if index >= max_attempts:
                    break
                continue

        report("正在上传并执行…")
        check_cancelled()
        try:
            attempt.outcome = await run_lua_and_collect(
                bridge,
                script,
                timeout=timeout,
                lua_timeout_ms=lua_timeout_ms,
                poll_timeout=poll_timeout,
                on_event=on_event,
                expects_long_run=long_run,
            )
        except Cancelled:
            # Cancelled 也是 RuntimeError，不单独接住的话会被下面那条
            # 归成"设备执行链路出错"——用户主动停止却看到一条设备报错。
            raise
        except Exception as err:
            attempt.error = f"设备执行链路出错：{err}"
            attempts.append(attempt)
            report(attempt.error)
            break

        attempts.append(attempt)
        outcome = attempt.outcome
        report(outcome.describe())

        if outcome.ok:
            if outcome.expects_long_run and outcome.status in {"running", "queued"}:
                report(f"执行成功：已稳定运行 {outcome.runtime_s}s，脚本仍在设备上跑着。")
            else:
                report("执行成功。")
            break
        if not outcome.retryable:
            if outcome.status == "unknown":
                attempt.stopped_because = "没能读到 job 状态，无法判断脚本对错，不再重试"
            elif outcome.status in {"running", "queued"}:
                attempt.stopped_because = (
                    f"等待 {poll_timeout:.0f}s 后 job 仍在 {outcome.status}，"
                    "脚本还在跑（长跑脚本属正常），不按失败重试"
                )
            else:
                attempt.stopped_because = (
                    f"status={outcome.status} 不属于需要改脚本的失败（长跑脚本或被主动停止），不再重试"
                )
            report(attempt.stopped_because)
            break
        if not outcome.has_diagnostics:
            attempt.stopped_because = "设备没有返回 summary 或日志，缺少可用于修脚本的信息，不再重试"
            report(attempt.stopped_because)
            break

        signature = outcome.error_signature()
        if signature == last_signature:
            attempt.stopped_because = f"连续两轮同一个错误（{signature}），LLM 没有收敛，不再重试"
            report(attempt.stopped_because)
            break
        last_signature = signature

    return attempts, plan


async def cmd_scan(args: argparse.Namespace) -> None:
    scanner = _import_bleak().BleakScanner
    devices = await scanner.discover(timeout=args.timeout, service_uuids=[SERVICE_UUID])
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
        outcome = await run_lua_and_collect(
            bridge,
            script,
            timeout=args.timeout,
            lua_timeout_ms=args.lua_timeout_ms,
            poll_timeout=args.poll_timeout,
            on_event=lambda message: print(message),
        )
        print("\n--- 执行结果 ---")
        print(outcome.describe())
        if not outcome.ok:
            raise SystemExit(1)

    await with_bridge(args, run)


async def cmd_ask(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    history = read_recent_history(args.history)
    report = lambda message: print(message)  # noqa: E731

    async def run(bridge: BleBridge) -> None:
        info = await bridge.request(
            {"id": request_id("info"), "op": "device_info"},
            timeout=args.timeout,
        )
        device_info = str(info.get("data") or info.get("error") or "")

        report("正在规划实现方案…")
        plan = await asyncio.to_thread(plan_for, config, args.prompt, device_info)
        report(plan.describe())

        if not args.yes and args.iterate <= 1:
            # 单轮模式保留"先看脚本再执行"的确认习惯。
            messages = build_messages(args.prompt, device_info, history, config.system_prompt, plan)
            raw = await asyncio.to_thread(
                call_llm,
                config,
                messages,
                on_progress=make_progress_reporter(report),
                on_note=report,
            )
            script = extract_lua(raw)
            validate_generated_lua(script)
            print("\n--- Generated Lua ---\n")
            print(script)
            print("\n--- End Lua ---\n")
            if input("上传并执行？[y/N] ").strip().lower() not in {"y", "yes"}:
                return
            outcome = await run_lua_and_collect(
                bridge,
                script,
                timeout=args.timeout,
                lua_timeout_ms=args.lua_timeout_ms,
                poll_timeout=args.poll_timeout,
                on_event=report,
                web_preview_check=args.web_preview_check,
            )
            attempts = [Attempt(index=1, script=script, raw=raw, outcome=outcome)]
        else:
            attempts, plan = await iterate_lua(
                bridge,
                config,
                args.prompt,
                device_info,
                history,
                plan=plan,
                max_attempts=args.iterate,
                timeout=args.timeout,
                lua_timeout_ms=args.lua_timeout_ms,
                poll_timeout=args.poll_timeout,
                on_event=report,
            )

        if not attempts:
            return

        final = attempts[-1]
        print("\n--- 最终 Lua ---\n")
        print(final.script)
        print("\n--- 结束 ---\n")
        print(summarize_attempts(attempts))

        append_history(
            args.history,
            {
                "created_at": int(time.time()),
                "prompt": args.prompt,
                "modules": plan.modules,
                "docs": plan.docs,
                "attempts": len(attempts),
                "raw": final.raw,
                "script": final.script,
                "device_info": device_info,
                "result": final.outcome.describe() if final.outcome else final.error,
                "ok": final.ok,
            },
        )
        if not final.ok:
            raise SystemExit(1)

    await with_bridge(args, run)


def summarize_attempts(attempts: list[Attempt]) -> str:
    """给出每一轮的结果，让人一眼看出 token 花在哪、为什么停下。"""
    lines = [f"共 {len(attempts)} 轮："]
    for attempt in attempts:
        if attempt.outcome:
            state = f"status={attempt.outcome.status}"
        elif attempt.error:
            state = f"未执行（{attempt.error}）"
        else:
            state = "未执行"
        lines.append(f"  第 {attempt.index} 轮：{state}")
        if attempt.stopped_because:
            lines.append(f"    停止原因：{attempt.stopped_because}")
    lines.append("结果：成功" if attempts[-1].ok else "结果：未成功")
    return "\n".join(lines)


async def cmd_plan(args: argparse.Namespace) -> None:
    """只做规划阶段，不连设备、不执行，用来检查计划和模块选择是否合理。"""
    config = load_config(args.config)
    plan = plan_for(config, args.prompt)
    context, docs = catalog.load_module_docs(plan.modules)
    plan.docs[:] = docs
    print(plan.describe())
    print(f"README 上下文大小：{len(context)} 字符")


def cmd_config(args: argparse.Namespace) -> None:
    current = load_config(args.config)
    provider = args.provider or current.provider
    preset = HOST_LLM_PRESETS.get(provider)
    from_preset = bool(args.provider and preset)

    next_config = HostLlmConfig(
        provider=provider,
        base_url=(args.base_url if args.base_url is not None else (preset.base_url if from_preset else current.base_url)),
        model=(args.model if args.model is not None else (preset.model if from_preset else current.model)),
        api_key=args.api_key if args.api_key is not None else current.api_key,
        auth_type=(args.auth_type if args.auth_type is not None else (preset.auth_type if from_preset else current.auth_type)),
        max_tokens=(args.max_tokens if args.max_tokens is not None else (preset.max_tokens if from_preset else current.max_tokens)),
        system_prompt=current.system_prompt,
        # 显式 --api-style 优先；否则跟着预设走（切到 Anthropic 必须换风格，
        # 否则会拿 OpenAI 的请求体去打 /v1/messages，必然 400）。
        api_style=(
            args.api_style
            if args.api_style is not None
            else (preset.api_style if from_preset else current.api_style)
        ),
        extra_headers=dict(current.extra_headers),
        extra_params=dict(current.extra_params),
    )
    save_config(args.config, next_config)
    safe = {**asdict(next_config), "api_key": "***" if next_config.api_key else ""}
    # 把归一后的地址一并打出来：Base URL 填法不统一是 404 的头号原因，
    # 而只看配置里的 base_url 是看不出实际会请求哪个路径的。
    safe["_resolved_endpoint"] = resolved_endpoint(next_config)
    print_json(safe)


def cmd_check(args: argparse.Namespace) -> None:
    """探活当前 LLM 配置，一次分清地址错 / Key 错 / 模型名错 / 网络不通。

    这条命令是为"请求失败但看不出为什么"准备的：不必再拿一次完整生成去试，
    也不用连设备。
    """
    config = load_config(args.config)
    print(f"配置文件：{args.config}")
    print(f"provider：{provider_label(config.provider)}    模型：{config.model}")
    print(check_connection(config))


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


async def cmd_apps(args: argparse.Namespace) -> None:
    from app_package import app_request, build_package, download_app, install_app, validate_identity
    package = None
    if args.command in {"install-app", "export-app"}:
        script = Path(args.script).read_text(encoding="utf-8")
        validate_generated_lua(script)
        package = build_package(script, args.app_id, args.title, selected=args.icon_selected,
                                unselected=args.icon_unselected, frame_color=args.color, pill_color=args.pill_color)
        if args.command == "export-app":
            print(package.export(args.apps_dir))
            return
    if args.command in {"remove-app", "rename-app", "app-info", "download-app"}:
        validate_identity(args.app_id, args.title if args.command == "rename-app" else "app")

    async def run(bridge: BleBridge) -> None:
        if args.command == "install-app":
            print_json(await install_app(bridge, package, print))
        elif args.command == "remove-app":
            print_json(await app_request(bridge, "app_remove", app_id=args.app_id))
        elif args.command == "rename-app":
            print_json(await app_request(bridge, "app_rename", app_id=args.app_id, title=args.title))
        elif args.command == "app-info":
            print_json(await app_request(bridge, "app_info", app_id=args.app_id))
        elif args.command == "download-app":
            downloaded = await download_app(bridge, args.app_id, print)
            print(downloaded.export(args.apps_dir))
        else:
            print_json(await app_request(bridge, "app_reload" if args.command == "reload-apps" else "app_list"))
    await with_bridge(args, run)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ESP-Claw 电脑端图形上位机")
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
    config.add_argument("--base-url",
                        help="接口地址。写到 /v1 或写完整 endpoint 都行；"
                             "末尾加 # 表示锁定该地址、不要追加路径")
    config.add_argument("--model")
    config.add_argument("--api-key")
    config.add_argument("--auth-type", choices=["bearer", "none"],
                        help="none 用于本地 vLLM/Ollama 这类不校验 Key 的服务")
    config.add_argument("--max-tokens", type=int)
    config.add_argument("--api-style", choices=list(API_STYLES),
                        help="接口风格：openai_chat=/chat/completions，"
                             "anthropic=/v1/messages（两者请求体和鉴权头都不同）")

    sub.add_parser("check", help="探活当前 LLM 配置，分清地址/Key/模型名/网络哪里有问题")

    ask = sub.add_parser("ask", help="自然语言生成 Lua，并通过 BLE 上传执行")
    ask.add_argument("prompt")
    ask.add_argument("--yes", action="store_true", help="跳过执行确认")
    ask.add_argument("--lua-timeout-ms", type=int, default=None,
                     help="设备端执行上限；不传则按任务类型自动定（游戏/界面不限时，"
                          "一次性脚本 60s）。0 = 跑到被停止为止")
    ask.add_argument("--poll-timeout", type=float, default=JOB_POLL_TIMEOUT,
                     help="等待 job 终态的最长秒数")
    ask.add_argument("--iterate", type=int, nargs="?", const=DEFAULT_MAX_ATTEMPTS, default=1,
                     metavar="N",
                     help=f"失败后按设备返回的报错自动迭代，最多 N 轮（不带值时为 {DEFAULT_MAX_ATTEMPTS}）")
    ask.add_argument("--web-preview-check", action="store_true",
                     help="每轮上传设备前先用真实 Web LVGL 模拟器 headless 运行，失败则先回灌给 LLM 修复")

    run_lua = sub.add_parser("run-lua", help="直接上传并执行本地 Lua 文件")
    run_lua.add_argument("script")
    run_lua.add_argument("--lua-timeout-ms", type=int, default=None,
                         help="设备端执行上限；不传则按脚本内容自动定（有主循环的不限时，"
                              "一次性脚本 60s）。0 = 跑到被停止为止")
    run_lua.add_argument("--poll-timeout", type=float, default=JOB_POLL_TIMEOUT,
                         help="等待 job 终态的最长秒数")

    plan = sub.add_parser("plan", help="只看模块路由结果，不连接设备")
    plan.add_argument("prompt")

    for command in ("install-app", "export-app"):
        app = sub.add_parser(command, help="安装到设备应用栏" if command == "install-app" else "导出到 SD 卡 apps 文件夹")
        app.add_argument("script")
        app.add_argument("--app-id", required=True)
        app.add_argument("--title", required=True)
        app.add_argument("--icon-selected", type=Path)
        app.add_argument("--icon-unselected", type=Path)
        app.add_argument("--color", type=lambda value: int(value.removeprefix("#"), 16), default=0xEDC22E)
        app.add_argument("--pill-color", type=lambda value: int(value.removeprefix("#"), 16))
        if command == "export-app":
            app.add_argument("--apps-dir", type=Path, required=True)
    sub.add_parser("list-apps", help="列出已安装应用")
    sub.add_parser("reload-apps", help="从 SD 卡重新加载应用栏")
    info = sub.add_parser("app-info", help="获取用户应用的名称、颜色、代码与图标路径及文件摘要")
    info.add_argument("app_id")
    download = sub.add_parser("download-app", help="通过蓝牙取回用户应用的配置、Lua 和图标")
    download.add_argument("app_id")
    download.add_argument("--apps-dir", type=Path, required=True)
    rename = sub.add_parser("rename-app", help="修改用户应用显示名称，保持ID和文件路径")
    rename.add_argument("app_id")
    rename.add_argument("title")
    remove = sub.add_parser("remove-app", help="删除已安装应用（先停止正在运行的应用）")
    remove.add_argument("app_id")
    sub.add_parser("modules", help="打印设备可用 Lua 模块清单")
    return parser


async def main_async(args: argparse.Namespace) -> None:
    if args.command in {"install-app", "export-app", "list-apps", "remove-app", "rename-app", "reload-apps", "app-info", "download-app"}:
        await cmd_apps(args)
    elif args.command == "scan":
        await cmd_scan(args)
    elif args.command == "hello":
        await cmd_hello(args)
    elif args.command == "info":
        await cmd_info(args)
    elif args.command == "ask":
        await cmd_ask(args)
    elif args.command == "run-lua":
        await cmd_run_lua(args)
    elif args.command == "plan":
        await cmd_plan(args)
    else:
        raise RuntimeError(f"未知命令：{args.command}")


def main() -> None:
    args = build_parser().parse_args()
    if args.command == "config":
        cmd_config(args)
        return
    if args.command == "modules":
        print(catalog.catalog_overview())
        return
    if args.command == "check":
        # 不连设备、不进事件循环：这条命令只做 HTTP 探活。
        cmd_check(args)
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

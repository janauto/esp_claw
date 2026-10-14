#!/usr/bin/env python3
"""LLM provider 层：OpenAI Chat Completions 与 Anthropic Messages 两条通路。

实现思路取自 tools/example_project/cursor-byok-main/server/src/provider/，
把那份 Rust 实现里几条"被真实服务教出来"的经验搬过来：

1. **存完整 endpoint，不存 base_url**（router.rs 的 `request_url`）。
   之前是裸拼 `f"{base_url}/chat/completions"`，用户把完整地址粘进来就变成
   `/chat/completions/chat/completions` → 404，而报错只说"请求失败"。

2. **SSE 流里的 error 帧要当错误处理**（mod.rs `provider_event_error`）。
   HTTP 200 之后服务照样能在流中间推 `{"error": {...}}`。之前这种帧被
   json 解析后直接忽略，最后报"LLM 返回为空"，把真正的原因盖掉了。

3. **非 2xx 必须把响应体带进错误**（attempt.rs `send_once`）。
   `unsupported_parameter` 这类信息全在 body 里，只报状态码等于没报。

4. **参数不兼容要按服务的回话降级重试**（mod.rs 的 allowlist / extra_params）。
   OpenAI 新模型只认 `max_completion_tokens`，vLLM/Ollama 之类不认
   `stream_options`。之前是"400 就退回非流式"，退回后同样的 400 再来一次，
   而且第二次的错误把第一次的细节覆盖了。这里改成读 400 的 body，
   精确地改一个字段再重试，并且始终保留最初那条错误。

5. **"总超时"和"静默超时"是两件事**（router.rs 的 `request_timeout` /
   `stream_idle_timeout`）。流式下只该判"多久没收到新字节"。

6. **stop_reason 的映射要保守**（openai_chat.rs `map_finish`）。
   `length` 永远是 length：截断的脚本能过语法但秒退，必须当场判失败。
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Iterator, Protocol
from urllib.parse import urlsplit, urlunsplit

import requests

#: 两种接口风格。openai_chat 覆盖所有 OpenAI 兼容服务（DeepSeek / Kimi /
#: 百炼 / vLLM / Ollama / 各类网关），anthropic 走原生 /v1/messages。
API_STYLE_OPENAI_CHAT = "openai_chat"
API_STYLE_ANTHROPIC = "anthropic"
API_STYLES: tuple[str, ...] = (API_STYLE_OPENAI_CHAT, API_STYLE_ANTHROPIC)

#: Anthropic 要求显式带版本头，缺了会 400。
ANTHROPIC_VERSION = "2023-06-01"

#: 各风格的规范路径。base_url 里已经带了就不再追加（见 resolve_endpoint）。
_ENDPOINT_PATHS: dict[str, str] = {
    API_STYLE_OPENAI_CHAT: "/chat/completions",
    API_STYLE_ANTHROPIC: "/v1/messages",
}
_MODELS_PATHS: dict[str, str] = {
    API_STYLE_OPENAI_CHAT: "/models",
    API_STYLE_ANTHROPIC: "/v1/models",
}

#: 建立连接的超时。和读超时分开：连不上应该几秒内就知道，
#: 不该跟着输出预算一起放大到几百秒。
DEFAULT_CONNECT_TIMEOUT_S = 20.0

#: 值得重试的瞬时故障。429 是限流，5xx 是服务端抖动，两者等一会儿再来有意义。
#: 4xx 里除了 429 都是请求本身的问题，重试只是白烧一次。
RETRYABLE_STATUS = frozenset({408, 409, 425, 429, 500, 502, 503, 504, 529})

#: 服务端在应用层推过来的过载/限流短语。只列语义非常明确的词，
#: 宁可少判不多判：误判会把截断等真正的失败当成可重试，盖掉真正的原因。
_TRANSIENT_PHRASES: tuple[str, ...] = (
    "overloaded",
    "rate limit",
    "rate_limit",
    "too many requests",
    "server is busy",
    "service unavailable",
    "capacity",
    "503",
)

def _is_transient_message(message: str) -> bool:
    low = message.lower()
    return any(p in low for p in _TRANSIENT_PHRASES)

#: 这些状态码意味着"请求本身有个字段服务不接受"，读 body 改字段后重试有救。
#: 401/403/429 刻意不在其中：鉴权和限流改字段治不好，重试只是白费一个请求，
#: 还会用第二次的失败盖掉第一次那条真正有用的错误。
DEGRADABLE_STATUS = frozenset({400, 404, 422, 500, 501})

#: 降级最多试几轮。每轮至多消掉一个字段且 tag 不重复，这个上限只是护栏。
MAX_DEGRADE_ROUNDS = 4
#: 瞬时故障最多补几次。给大了会把"服务挂了"拖成几分钟的假等待。
MAX_TRANSIENT_RETRIES = 2
#: 应用层过载（"overloaded"/"rate limit"）的最大重试次数。
#: 比纯 HTTP 5xx 给多一次：这类错误常见于繁忙时段，稍等一下就能过。
MAX_OVERLOAD_RETRIES = 4
#: 过载重试的退避基数。
OVERLOAD_BACKOFF_BASE_S = 3.0
#: 没有 Retry-After 时的退避基数，实际等待是 base * 2**n。
RETRY_BACKOFF_BASE_S = 1.5
#: 尊重 Retry-After，但不无上限地等。
RETRY_AFTER_CAP_S = 30.0


class Cancelled(RuntimeError):
    """用户主动取消了这次生成。

    和"失败"分开：取消不该弹错误框，也不该触发迭代循环的修复分支。
    """


class TransientError(RuntimeError):
    """服务端短暂过载、限流或上游抖动，过一会儿重试有意义。

    和永久失败（Key 错、模型名错、请求体字段不对）分开：
    靠字符串匹配来区分"能重试"和"不能重试"会把截断等正常失败也误判，
    所以只有明确属于过载/限流语义的错误才能装进这个类型。
    """


class StreamUnsupported(RuntimeError):
    """服务不接受流式，可以退回非流式重试。

    保留成独立类型而不是匹配报错字符串：靠字符串会把"输出被截断"之类的
    正常失败也误判成可重试，白烧一次请求还把真正的原因盖掉。
    """


class LlmHttpError(RuntimeError):
    """带状态码和响应体的 HTTP 失败。

    降级逻辑要读 body 才能知道是哪个字段不被接受，所以这些信息必须留在
    异常上，不能在构造消息时就丢掉。
    """

    def __init__(self, label: str, status: int, body: str, url: str) -> None:
        self.status = status
        self.body = body
        self.url = url
        super().__init__(f"{label} 请求失败：HTTP {status} {body.strip()[:1200]}")

    @property
    def detail(self) -> str:
        """小写化的 body，专门给降级判断做子串匹配用。"""
        return self.body.lower()


class Cancellation(Protocol):
    """只要有 is_set() 就能当取消令牌，threading.Event 直接可用。"""

    def is_set(self) -> bool: ...


class LlmConfigLike(Protocol):
    """provider 层只认这几个字段，不依赖 host_vibe 的 dataclass（避免循环导入）。"""

    base_url: str
    model: str
    api_key: str
    auth_type: str
    max_tokens: int


@dataclass(frozen=True)
class Endpoint:
    """一次请求的目标：完整 URL + 请求头 + 接口风格。"""

    url: str
    headers: dict[str, str]
    api_style: str
    label: str


@dataclass
class Usage:
    """token 用量。字段名对齐 cursor-byok 的 model::Usage。"""

    input_tokens: int | None = None
    output_tokens: int | None = None
    total_tokens: int | None = None
    cache_read_tokens: int | None = None
    cache_write_tokens: int | None = None
    reasoning_tokens: int | None = None

    def describe(self) -> str:
        parts = [
            f"输入 {self.input_tokens}" if self.input_tokens is not None else "",
            f"输出 {self.output_tokens}" if self.output_tokens is not None else "",
            f"缓存命中 {self.cache_read_tokens}" if self.cache_read_tokens else "",
            f"思维链 {self.reasoning_tokens}" if self.reasoning_tokens else "",
        ]
        return "，".join(part for part in parts if part)


@dataclass
class Delta:
    """从一帧 SSE 里翻译出来的增量。两种风格都归一到这个形状。"""

    text: str = ""
    reasoning: str = ""
    finish: str = ""
    usage: Usage | None = None
    done: bool = False


@dataclass
class LlmResult:
    """一次完整生成的结果。"""

    text: str = ""
    reasoning: str = ""
    finish: str = ""
    usage: Usage = field(default_factory=Usage)
    api_style: str = API_STYLE_OPENAI_CHAT
    #: 为了让这次请求成功而做过的降级，用于回报给用户和写进配置建议。
    degraded: list[str] = field(default_factory=list)
    #: 是否退回了非流式。
    streamed: bool = True


# ---------------------------------------------------------------------------
# endpoint 解析
# ---------------------------------------------------------------------------


def infer_api_style(base_url: str) -> str:
    """从 base_url 猜接口风格，只用于兼容没有 api_style 字段的旧配置。"""
    host_and_path = base_url.lower()
    if "api.anthropic.com" in host_and_path or "/v1/messages" in host_and_path:
        return API_STYLE_ANTHROPIC
    return API_STYLE_OPENAI_CHAT


def resolve_endpoint_url(base_url: str, api_style: str) -> str:
    """把用户填的 Base URL 归一成可直接 POST 的完整地址。

    这是"有的请求失败"里最不讲道理的一类：用户从别的工具里复制了完整
    endpoint 粘进 Base URL，裸拼之后路径重复，服务回 404，而界面只说
    "请求失败"。cursor-byok 的做法是配置里直接存完整 request_url
    （router.rs `model.request_url()`），这里做等价的事——但对两种写法都宽容：

    - `https://api.deepseek.com/v1`                 -> 追加 /chat/completions
    - `https://api.deepseek.com/v1/chat/completions` -> 原样使用
    - `https://gw.example.com/openai/v1/`            -> 追加，多余的斜杠归一
    - `https://gw.example.com/custom/path#`          -> 末尾 # 表示"就用这个地址"

    末尾 `#` 是这类工具的通用约定：网关的实际路径可能既不像 /v1 也不像
    /chat/completions，需要一个"别猜了"的开关。
    """
    raw = (base_url or "").strip()
    if not raw:
        raise RuntimeError("请先配置 Base URL")

    if raw.endswith("#"):
        # 显式锁定：去掉标记后原样使用。
        return raw[:-1].rstrip("/") or raw[:-1]

    if "://" not in raw:
        # 少了 scheme 是很常见的手滑，补 https 比报错有用。
        raw = f"https://{raw}"

    split = urlsplit(raw)
    path = split.path.rstrip("/")
    suffix = _ENDPOINT_PATHS[api_style]

    if path.endswith(suffix):
        resolved_path = path
    elif api_style == API_STYLE_ANTHROPIC and path.endswith("/messages"):
        # 有些代理把 anthropic 挂在 /anthropic/messages 之类的路径下。
        resolved_path = path
    elif api_style == API_STYLE_ANTHROPIC and not path:
        resolved_path = suffix
    elif api_style == API_STYLE_ANTHROPIC:
        # base_url 已经带了 /v1 时不要拼成 /v1/v1/messages。
        resolved_path = f"{path}/messages" if path.endswith("/v1") else f"{path}{suffix}"
    else:
        resolved_path = f"{path}{suffix}"

    return urlunsplit((split.scheme, split.netloc, resolved_path, "", ""))


def resolve_models_url(base_url: str, api_style: str) -> str:
    """/models 地址，给"测试连接"用。"""
    endpoint = resolve_endpoint_url(base_url, api_style)
    suffix = _ENDPOINT_PATHS[api_style]
    if endpoint.endswith(suffix):
        root = endpoint[: -len(suffix)]
    else:
        root = endpoint.rstrip("/")
    if api_style == API_STYLE_ANTHROPIC:
        return f"{root}{_MODELS_PATHS[api_style]}" if not root.endswith("/v1") else f"{root}/models"
    return f"{root}{_MODELS_PATHS[api_style]}"


def api_style_of(config: LlmConfigLike) -> str:
    style = str(getattr(config, "api_style", "") or "").strip()
    if style in API_STYLES:
        return style
    return infer_api_style(getattr(config, "base_url", "") or "")


def resolve_endpoint(config: LlmConfigLike) -> Endpoint:
    """组装这次请求的 URL 和头。

    鉴权头按风格走，不再让 auth_type 单独决定：Anthropic 用 x-api-key，
    OpenAI 系用 Authorization: Bearer。auth_type="none" 仍然表示"不带鉴权"，
    本地 vLLM / Ollama 需要这个。
    """
    api_style = api_style_of(config)
    auth_type = str(getattr(config, "auth_type", "bearer") or "bearer")
    api_key = str(getattr(config, "api_key", "") or "")

    headers: dict[str, str] = {"Content-Type": "application/json"}
    label = "Anthropic" if api_style == API_STYLE_ANTHROPIC else "OpenAI 兼容"

    if api_style == API_STYLE_ANTHROPIC:
        headers["anthropic-version"] = ANTHROPIC_VERSION
        if auth_type != "none":
            headers["x-api-key"] = api_key
    elif auth_type != "none":
        headers["Authorization"] = f"Bearer {api_key}"

    extra = getattr(config, "extra_headers", None)
    if isinstance(extra, dict):
        for name, value in extra.items():
            if isinstance(name, str) and isinstance(value, str) and name.strip():
                headers[name.strip()] = value

    return Endpoint(
        url=resolve_endpoint_url(getattr(config, "base_url", ""), api_style),
        headers=headers,
        api_style=api_style,
        label=label,
    )


# ---------------------------------------------------------------------------
# 请求体
# ---------------------------------------------------------------------------


def _split_system(messages: list[dict[str, str]]) -> tuple[str, list[dict[str, str]]]:
    """Anthropic 的 system 是独立字段，不能混在 messages 里。"""
    system_parts: list[str] = []
    rest: list[dict[str, str]] = []
    for message in messages:
        if str(message.get("role")) == "system":
            content = str(message.get("content") or "")
            if content.strip():
                system_parts.append(content)
            continue
        rest.append(message)
    return "\n\n".join(system_parts), rest


def _merge_adjacent(messages: list[dict[str, str]]) -> list[dict[str, str]]:
    """把连续同 role 的消息合并。

    Anthropic 不接受连续两条 user，会直接 400。上下文引擎按段落产出
    提示词，很自然就会出现连续 user——这是"换成 Claude 就全失败"的常见根因。
    照 anthropic.rs `push_anthropic` 的做法合并。
    """
    out: list[dict[str, str]] = []
    for message in messages:
        role = str(message.get("role") or "user")
        content = str(message.get("content") or "")
        if out and out[-1]["role"] == role:
            out[-1]["content"] = f"{out[-1]['content']}\n\n{content}".strip()
            continue
        out.append({"role": role, "content": content})
    return [item for item in out if item["content"].strip()]


def build_body(
    config: LlmConfigLike,
    messages: list[dict[str, str]],
    budget: int,
    *,
    api_style: str,
    stream: bool,
) -> dict[str, Any]:
    """按风格组装请求体。字段选择上刻意保守：只发必需的。

    额外参数一律走配置里的 extra_params，而不是在这里硬塞：
    多发一个字段就多一个被某家服务拒收的机会，而这正是原来失败的原因之一。
    """
    model = str(getattr(config, "model", "") or "")

    if api_style == API_STYLE_ANTHROPIC:
        system, rest = _split_system(messages)
        body: dict[str, Any] = {
            "model": model,
            # Anthropic 的 max_tokens 是必填字段，不是可选上限。
            "max_tokens": budget,
            "messages": _merge_adjacent(rest),
        }
        if system:
            body["system"] = system
        if stream:
            body["stream"] = True
    else:
        body = {
            "model": model,
            "max_tokens": budget,
            "messages": messages,
        }
        if stream:
            body["stream"] = True
            # 有 usage 才能在截断时报出真实的 completion_tokens。
            body["stream_options"] = {"include_usage": True}

    extra = getattr(config, "extra_params", None)
    if isinstance(extra, dict):
        # 照 mod.rs merge_extra_params：用户可以加 temperature/top_p 之类，
        # 但不能顶掉决定语义的字段。
        protected = {"model", "messages", "stream", "system", "max_tokens", "max_completion_tokens"}
        for name, value in extra.items():
            if name in protected:
                raise RuntimeError(f"extra_params 不允许覆盖 {name}")
            body[name] = value

    return body


# ---------------------------------------------------------------------------
# 参数降级：读 400 的 body，改一个字段再来
# ---------------------------------------------------------------------------


#: 一次降级的结果：稳定的 tag（判重 + 重放用）、人类可读说明、新 body。
#: tag 和说明分开，是为了让说明的措辞可以随便改而不影响判重逻辑。
@dataclass(frozen=True)
class Degradation:
    tag: str
    label: str
    body: dict[str, Any]


#: 决定语义的字段，任何降级都不许动。
_ESSENTIAL_FIELDS = frozenset({"model", "messages", "stream"})


def degrade_body(
    body: dict[str, Any],
    error: LlmHttpError,
    applied: list[str],
) -> Degradation | None:
    """按服务的拒绝理由改一个字段，返回这次降级；没有可试的就返回 None。

    这替代了原来"400/404/422/501 一律退回非流式"的粗判。那套逻辑有两个问题：
    - 对 `max_completion_tokens` 这类真正的字段不兼容无效，退回非流式后
      同样的 400 再来一次；
    - 第二次失败的消息会盖掉第一次，用户看到的报错和真实原因无关。

    返回 None 时调用方应该抛出**最初**那条错误，而不是拿一个更含糊的覆盖它。
    """
    detail = error.detail
    changed = dict(body)

    def unused(tag: str) -> bool:
        return tag not in applied

    # OpenAI 新模型（o 系列 / gpt-5 系）只认 max_completion_tokens。
    if (
        "max_tokens" in changed
        and unused("max_completion_tokens")
        and ("max_completion_tokens" in detail or "max_tokens" in detail)
    ):
        changed["max_completion_tokens"] = changed.pop("max_tokens")
        return Degradation(
            "max_completion_tokens", "改用 max_completion_tokens（服务不接受 max_tokens）", changed
        )

    # 反向：部分兼容层只认老字段。
    if (
        "max_completion_tokens" in changed
        and unused("max_tokens")
        and "max_completion_tokens" in detail
    ):
        changed["max_tokens"] = changed.pop("max_completion_tokens")
        return Degradation(
            "max_tokens", "改用 max_tokens（服务不接受 max_completion_tokens）", changed
        )

    # vLLM / Ollama / 一些网关不认 stream_options。
    if "stream_options" in changed and unused("stream_options") and "stream_options" in detail:
        changed.pop("stream_options")
        return Degradation("stream_options", "去掉 stream_options（服务不支持用量回传）", changed)

    # 报错里点名了某个额外参数（temperature / reasoning_effort / …）。
    for name in list(changed):
        if name in _ESSENTIAL_FIELDS:
            continue
        if unused(f"drop:{name}") and f"'{name}'" in detail and "unsupported" in detail:
            changed.pop(name)
            return Degradation(f"drop:{name}", f"去掉不被支持的参数 {name}", changed)

    # 兜底：很多兼容层的 400 body 并不点名字段，只回一句 "invalid request"。
    # stream_options 是最可能的元凶，无条件去掉再试一次。
    if "stream_options" in changed and unused("stream_options"):
        changed.pop("stream_options")
        return Degradation("stream_options", "去掉 stream_options 后重试", changed)

    return None


# ---------------------------------------------------------------------------
# SSE
# ---------------------------------------------------------------------------


def iter_sse(response: requests.Response, cancel: Cancellation | None) -> Iterator[tuple[str, str]]:
    """把字节流切成 (event 名, data) 对。

    不用 requests 的 iter_lines：它按固定 chunk 读，会在没有换行的时候
    攒住数据，静默间隔的判断就不准了。这里自己缓冲，收到多少处理多少。
    """
    event_name = ""
    data_lines: list[str] = []
    buffer = b""

    def flush() -> tuple[str, str] | None:
        nonlocal event_name, data_lines
        if not data_lines:
            event_name = ""
            return None
        payload = "\n".join(data_lines)
        name = event_name
        event_name = ""
        data_lines = []
        return name, payload

    for chunk in response.iter_content(chunk_size=None):
        if cancel is not None and cancel.is_set():
            raise Cancelled("生成已取消")
        if not chunk:
            continue
        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").rstrip("\r")

            if not line:
                event = flush()
                if event:
                    yield event
                continue
            if line.startswith(":"):
                # 注释帧（": ping"）没有内容，但足以说明连接是活的。
                yield "", ""
                continue
            if line.startswith("event:"):
                event_name = line[len("event:"):].strip()
                continue
            if line.startswith("data:"):
                if data_lines and not event_name:
                    # 有些网关不发空行分隔。不宽容的话整个流会被当成一帧。
                    event = flush()
                    if event:
                        yield event
                data_lines.append(line[len("data:"):].strip())
                continue
            # 其他字段（id: / retry:）用不上，但同样说明连接是活的。
            yield "", ""

    event = flush()
    if event:
        yield event


# ---------------------------------------------------------------------------
# 事件翻译
# ---------------------------------------------------------------------------


def provider_event_error(label: str, value: Any) -> str | None:
    """从一帧里认出服务端推来的错误。

    直接照搬 mod.rs 的 provider_event_error：HTTP 200 之后服务照样会在流
    中间推 error 帧，而且藏的位置有好几种。之前这些帧被忽略，最后报
    "LLM 返回为空"——真正的原因（限流、上游超时、内容过滤）全丢了。
    """
    if not isinstance(value, dict):
        return None

    kind = value.get("type")
    direct = value.get("error")
    if kind not in {"error", "response.failed"} and direct in (None, {}, ""):
        return None

    candidates: list[Any] = [
        value.get("message"),
        (direct or {}).get("message") if isinstance(direct, dict) else None,
        ((value.get("response") or {}).get("error") or {}).get("message")
        if isinstance(value.get("response"), dict)
        else None,
        direct if isinstance(direct, str) else None,
    ]
    for candidate in candidates:
        if isinstance(candidate, str) and candidate.strip():
            return f"{label} 返回错误：{candidate.strip()}"
    return f"{label} 返回了一个没有消息体的错误帧"


def map_openai_finish(reason: str) -> str:
    """归一 finish_reason。

    照 openai_chat.rs map_finish 的保守取向：`length` 永远是 length。
    截断的脚本语法上能过、上传后 1 秒就退（屏幕闪一下回主界面），
    必须当场判失败，不能当成正常输出交给下游。
    """
    if reason in {"length", "max_tokens", "model_context_window_exceeded"}:
        return "length"
    if reason in {"tool_calls", "function_call", "tool_use"}:
        return "tool_use"
    if reason in {"content_filter"}:
        return "content_filter"
    if reason:
        return "stop"
    return ""


def openai_usage(value: Any) -> Usage | None:
    if not isinstance(value, dict):
        return None
    details = value.get("completion_tokens_details")
    prompt_details = value.get("prompt_tokens_details")
    return Usage(
        input_tokens=value.get("prompt_tokens"),
        output_tokens=value.get("completion_tokens"),
        total_tokens=value.get("total_tokens"),
        cache_read_tokens=(prompt_details or {}).get("cached_tokens")
        if isinstance(prompt_details, dict)
        else None,
        reasoning_tokens=(details or {}).get("reasoning_tokens")
        if isinstance(details, dict)
        else None,
    )


def anthropic_usage(value: Any) -> Usage | None:
    """Anthropic 的 usage 分散在 message_start 和 message_delta 两处。

    照 anthropic.rs anthropic_usage：cache_read/cache_creation 要算进
    "这次实际喂进去的上下文"，否则命中缓存时看起来像只发了几百 token。
    """
    if not isinstance(value, dict):
        return None
    input_tokens = value.get("input_tokens")
    cache_read = value.get("cache_read_input_tokens")
    cache_write = value.get("cache_creation_input_tokens")
    total_input = None
    if input_tokens is not None:
        total_input = input_tokens + (cache_read or 0) + (cache_write or 0)
    return Usage(
        input_tokens=total_input if total_input is not None else input_tokens,
        output_tokens=value.get("output_tokens"),
        cache_read_tokens=cache_read,
        cache_write_tokens=cache_write,
    )


def merge_usage(total: Usage, update: Usage | None) -> None:
    """按字段取 max 合并。

    照 anthropic.rs merge_usage：流式里同一个字段会被报好几次，
    后一次可能是 0（缺省值），直接覆盖会把真实数字擦掉。
    """
    if update is None:
        return
    for name in (
        "input_tokens",
        "output_tokens",
        "cache_read_tokens",
        "cache_write_tokens",
        "reasoning_tokens",
    ):
        new = getattr(update, name)
        if new is None:
            continue
        current = getattr(total, name)
        setattr(total, name, new if current is None else max(current, new))
    if total.input_tokens is not None and total.output_tokens is not None:
        total.total_tokens = total.input_tokens + total.output_tokens
    elif update.total_tokens is not None:
        total.total_tokens = max(total.total_tokens or 0, update.total_tokens)


def translate_openai_chunk(value: Any) -> Delta:
    delta = Delta()
    if not isinstance(value, dict):
        return delta

    delta.usage = openai_usage(value.get("usage"))

    choices = value.get("choices")
    if not isinstance(choices, list) or not choices:
        return delta
    choice = choices[0] if isinstance(choices[0], dict) else {}

    reason = choice.get("finish_reason") or choice.get("stop_reason") or ""
    if isinstance(reason, str) and reason:
        delta.finish = map_openai_finish(reason)

    payload = choice.get("delta")
    if not isinstance(payload, dict):
        # 少数兼容层在流式里直接给 message（而不是 delta）。
        payload = choice.get("message") if isinstance(choice.get("message"), dict) else {}

    text = payload.get("content")
    if isinstance(text, str):
        delta.text = text
    elif isinstance(text, list):
        # 有些服务把 content 做成分段数组。
        delta.text = "".join(
            str(part.get("text") or "") for part in text if isinstance(part, dict)
        )

    think = payload.get("reasoning_content")
    if not isinstance(think, str):
        think = payload.get("reasoning")
    if isinstance(think, str):
        delta.reasoning = think

    return delta


def translate_anthropic_event(name: str, value: Any) -> Delta:
    """把 Anthropic 的事件流翻译成统一的 Delta。

    事件形状照 anthropic.rs 的 match：类型既可能在 SSE 的 event 行，
    也可能只在 data 的 "type" 字段里，两处都要看。
    """
    delta = Delta()
    if not isinstance(value, dict):
        return delta

    kind = value.get("type") or name

    if kind == "message_start":
        message = value.get("message")
        if isinstance(message, dict):
            delta.usage = anthropic_usage(message.get("usage"))
        return delta

    if kind == "content_block_delta":
        payload = value.get("delta")
        if not isinstance(payload, dict):
            return delta
        block_kind = payload.get("type")
        if block_kind == "text_delta":
            delta.text = str(payload.get("text") or "")
        elif block_kind == "thinking_delta":
            delta.reasoning = str(payload.get("thinking") or "")
        return delta

    if kind == "message_delta":
        delta.usage = anthropic_usage(value.get("usage"))
        stop = (value.get("delta") or {}).get("stop_reason")
        if isinstance(stop, str) and stop:
            delta.finish = map_openai_finish(stop)
        return delta

    if kind == "message_stop":
        delta.done = True
        return delta

    return delta


# ---------------------------------------------------------------------------
# 非流式解析
# ---------------------------------------------------------------------------


def parse_openai_response(payload: Any) -> tuple[str, str, str, Usage]:
    if not isinstance(payload, dict):
        raise RuntimeError("LLM 返回的不是 JSON 对象")
    choices = payload.get("choices")
    choice = choices[0] if isinstance(choices, list) and choices and isinstance(choices[0], dict) else {}
    message = choice.get("message") if isinstance(choice.get("message"), dict) else {}

    text = message.get("content")
    if isinstance(text, list):
        text = "".join(str(part.get("text") or "") for part in text if isinstance(part, dict))
    reasoning = message.get("reasoning_content")
    if not isinstance(reasoning, str):
        reasoning = message.get("reasoning")

    return (
        text if isinstance(text, str) else "",
        reasoning if isinstance(reasoning, str) else "",
        map_openai_finish(str(choice.get("finish_reason") or "")),
        openai_usage(payload.get("usage")) or Usage(),
    )


def parse_anthropic_response(payload: Any) -> tuple[str, str, str, Usage]:
    if not isinstance(payload, dict):
        raise RuntimeError("LLM 返回的不是 JSON 对象")
    text_parts: list[str] = []
    think_parts: list[str] = []
    for block in payload.get("content") or []:
        if not isinstance(block, dict):
            continue
        if block.get("type") == "text":
            text_parts.append(str(block.get("text") or ""))
        elif block.get("type") == "thinking":
            think_parts.append(str(block.get("thinking") or ""))
    return (
        "".join(text_parts),
        "".join(think_parts),
        map_openai_finish(str(payload.get("stop_reason") or "")),
        anthropic_usage(payload.get("usage")) or Usage(),
    )


# ---------------------------------------------------------------------------
# 发送
# ---------------------------------------------------------------------------


def _retry_after_seconds(response: requests.Response, attempt: int) -> float:
    raw = response.headers.get("Retry-After", "")
    try:
        return min(RETRY_AFTER_CAP_S, max(0.0, float(raw)))
    except (TypeError, ValueError):
        return min(RETRY_AFTER_CAP_S, RETRY_BACKOFF_BASE_S * (2**attempt))


def _post(
    session: requests.Session,
    endpoint: Endpoint,
    body: dict[str, Any],
    *,
    stream: bool,
    connect_timeout: float,
    read_timeout: float,
    cancel: Cancellation | None,
    on_note: Callable[[str], None] | None,
) -> requests.Response:
    """发一次请求，成功返回 Response，非 2xx 抛 LlmHttpError。

    照 attempt.rs send_once：非 2xx 一定要把响应体读出来带进错误。
    429/5xx 在这里做有限重试——限流和上游抖动等一会儿确实会好。
    """
    payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
    last: LlmHttpError | Exception | None = None

    for attempt in range(MAX_TRANSIENT_RETRIES + 1):
        if cancel is not None and cancel.is_set():
            raise Cancelled("生成已取消")
        try:
            response = session.post(
                endpoint.url,
                data=payload,
                headers=endpoint.headers,
                stream=stream,
                timeout=(connect_timeout, read_timeout),
            )
        except requests.exceptions.ConnectTimeout as err:
            last = RuntimeError(
                f"连接 {endpoint.url} 超时（{connect_timeout:.0f}s）。"
                "请检查网络、代理，或 Base URL 是否可达。"
            )
            last.__cause__ = err
        except requests.exceptions.ReadTimeout as err:
            last = RuntimeError(
                f"LLM 请求超时：{read_timeout:.0f}s 内没有收到数据。"
                "可以调小 max_tokens，或换更快的服务。"
            )
            last.__cause__ = err
        except requests.exceptions.SSLError as err:
            last = RuntimeError(f"TLS 握手失败：{err}。若走的是自签证书代理，请配置 CA。")
        except requests.exceptions.ProxyError as err:
            last = RuntimeError(f"代理不可用：{err}。检查 HTTPS_PROXY / ALL_PROXY 设置。")
        except requests.exceptions.ConnectionError as err:
            last = RuntimeError(f"LLM 连接失败：{err}（url={endpoint.url}）")
        else:
            if response.status_code < 400:
                return response

            # 失败的响应一定要读完并关掉。stream=True 下不关会把连接留在
            # "已借出"状态，同一个 session 的下一次请求（降级重试、非流式回退）
            # 只能另开连接，在连接数受限的代理后面就会挂住。
            try:
                detail = response.text
            finally:
                response.close()

            if response.status_code in RETRYABLE_STATUS and attempt < MAX_TRANSIENT_RETRIES:
                wait = _retry_after_seconds(response, attempt)
                if on_note:
                    on_note(
                        f"服务返回 HTTP {response.status_code}，{wait:.1f}s 后重试"
                        f"（第 {attempt + 1}/{MAX_TRANSIENT_RETRIES} 次）"
                    )
                _sleep_cancellable(wait, cancel)
                continue

            raise LlmHttpError(endpoint.label, response.status_code, detail, endpoint.url)

        # 走到这里说明是传输层故障。
        if attempt < MAX_TRANSIENT_RETRIES:
            wait = RETRY_BACKOFF_BASE_S * (2**attempt)
            if on_note:
                on_note(f"{last}；{wait:.1f}s 后重试（第 {attempt + 1}/{MAX_TRANSIENT_RETRIES} 次）")
            _sleep_cancellable(wait, cancel)
            continue
        raise last

    raise last if last else RuntimeError("LLM 请求失败")


def _sleep_cancellable(seconds: float, cancel: Cancellation | None) -> None:
    """退避期间也要能被取消，否则"停止生成"要等满退避时间才生效。"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if cancel is not None and cancel.is_set():
            raise Cancelled("生成已取消")
        time.sleep(min(0.2, max(0.0, deadline - time.monotonic())))


def _stream_once(
    session: requests.Session,
    endpoint: Endpoint,
    body: dict[str, Any],
    *,
    connect_timeout: float,
    read_timeout: float,
    on_progress: Callable[[str, bool], None] | None,
    on_note: Callable[[str], None] | None,
    cancel: Cancellation | None,
) -> LlmResult:
    """流式读一次。

    流式的价值不只是"界面有东西动"：超时的语义从"总耗时"变成"静默间隔"，
    模型写得慢（正常）和连接断了（异常）从此可以区分。
    """
    response = _post(
        session,
        endpoint,
        body,
        stream=True,
        connect_timeout=connect_timeout,
        read_timeout=read_timeout,
        cancel=cancel,
        on_note=on_note,
    )

    content: list[str] = []
    reasoning: list[str] = []
    result = LlmResult(api_style=endpoint.api_style, streamed=True)
    saw_done = False

    try:
        if on_progress:
            on_progress("", False)
        for name, data in iter_sse(response, cancel):
            if not data:
                # 保活帧：没有内容，但连接是活的。
                if on_progress:
                    on_progress("", False)
                continue
            if data == "[DONE]":
                saw_done = True
                break

            try:
                value = json.loads(data)
            except json.JSONDecodeError:
                continue

            problem = provider_event_error(endpoint.label, value)
            if problem:
                if _is_transient_message(problem):
                    raise TransientError(problem)
                raise RuntimeError(problem)

            if endpoint.api_style == API_STYLE_ANTHROPIC:
                delta = translate_anthropic_event(name, value)
            else:
                delta = translate_openai_chunk(value)

            merge_usage(result.usage, delta.usage)
            if delta.finish:
                result.finish = delta.finish
            if delta.done:
                saw_done = True
            if delta.reasoning:
                reasoning.append(delta.reasoning)
                if on_progress:
                    on_progress(delta.reasoning, True)
            if delta.text:
                content.append(delta.text)
                if on_progress:
                    on_progress(delta.text, False)
    finally:
        response.close()

    result.text = "".join(content)
    result.reasoning = "".join(reasoning)

    if not result.text and not result.reasoning and not saw_done and not result.finish:
        # 连接开了但一个可用帧都没有：多半是把非 SSE 响应当流读了。
        raise StreamUnsupported(f"{endpoint.label} 没有返回任何流式内容")

    return result


def _complete_once(
    session: requests.Session,
    endpoint: Endpoint,
    body: dict[str, Any],
    *,
    connect_timeout: float,
    read_timeout: float,
    on_note: Callable[[str], None] | None,
    cancel: Cancellation | None,
) -> LlmResult:
    response = _post(
        session,
        endpoint,
        body,
        stream=False,
        connect_timeout=connect_timeout,
        read_timeout=read_timeout,
        cancel=cancel,
        on_note=on_note,
    )
    try:
        raw = response.text
    finally:
        response.close()

    try:
        payload = json.loads(raw)
    except json.JSONDecodeError as err:
        raise RuntimeError(
            f"{endpoint.label} 返回的不是 JSON（前 300 字符）：{raw[:300]}"
        ) from err

    problem = provider_event_error(endpoint.label, payload)
    if problem:
        if _is_transient_message(problem):
            raise TransientError(problem)
        raise RuntimeError(problem)

    if endpoint.api_style == API_STYLE_ANTHROPIC:
        text, reasoning, finish, usage = parse_anthropic_response(payload)
    else:
        text, reasoning, finish, usage = parse_openai_response(payload)

    return LlmResult(
        text=text,
        reasoning=reasoning,
        finish=finish,
        usage=usage,
        api_style=endpoint.api_style,
        streamed=False,
    )


def _replay_degradations(applied: list[str], body: dict[str, Any]) -> dict[str, Any]:
    """把已确认的降级重放到新 body 上。

    重建 body（流式 -> 非流式）时不能丢掉"这个服务只认 max_completion_tokens"
    这类已经用一次失败换来的知识，否则回退请求会以同一个理由再失败一次。
    """
    for tag in applied:
        if tag == "max_completion_tokens" and "max_tokens" in body:
            body["max_completion_tokens"] = body.pop("max_tokens")
        elif tag == "max_tokens" and "max_completion_tokens" in body:
            body["max_tokens"] = body.pop("max_completion_tokens")
        elif tag.startswith("drop:"):
            body.pop(tag[len("drop:"):], None)
        # stream_options 不用重放：非流式的 body 本来就不带它。
    return body


def generate(
    config: LlmConfigLike,
    messages: list[dict[str, str]],
    *,
    max_tokens: int,
    stream_read_timeout: float,
    total_read_timeout: float,
    connect_timeout: float = DEFAULT_CONNECT_TIMEOUT_S,
    on_progress: Callable[[str, bool], None] | None = None,
    on_note: Callable[[str], None] | None = None,
    cancel: Cancellation | None = None,
    session: requests.Session | None = None,
) -> LlmResult:
    """跑一次生成，带参数降级和流式→非流式回退。

    顺序是有讲究的：
    1. 先按配置的风格流式发；
    2. 服务因为某个字段拒收 -> 读 body 精确改那个字段，重新流式发；
    3. 确认服务不支持流式 -> 带着已确认的降级退回非流式；
    4. 全都不行 -> 抛**最初**那条错误。

    第 4 步是重点。原来的实现在回退失败后抛的是第二次的错误，于是
    "模型名写错了" 会显示成 "返回为空"、"Key 过期" 会显示成 "连接失败"——
    报错和真实原因无关，这是之前最难排查的一点。
    """
    endpoint = resolve_endpoint(config)
    if not getattr(config, "model", ""):
        raise RuntimeError("请先配置 model")
    if str(getattr(config, "auth_type", "bearer")) != "none" and not getattr(config, "api_key", ""):
        raise RuntimeError("请先配置 API Key，或把 auth_type 设为 none")

    owned = session is None
    session = session or requests.Session()
    #: 已确认的降级 tag（判重 + 重放）与对应的人类可读说明。
    applied: list[str] = []
    labels: list[str] = []
    first_error: Exception | None = None

    def note(message: str) -> None:
        if on_note:
            on_note(message)

    try:
        body = build_body(
            config, messages, max_tokens, api_style=endpoint.api_style, stream=True
        )

        # Outer loop: overload / rate-limit retries.
        # These arrive as application-layer errors inside the stream body (HTTP 200
        # but the SSE data contains {"error": "overloaded"}). They are distinct from
        # parameter-shape errors (which only need to be degraded once) so they get
        # their own counter and backoff.
        for overload_attempt in range(MAX_OVERLOAD_RETRIES + 1):
            try:
                # Inner loop: field-shape degradation. Each round removes one
                # disallowed field; tags never repeat, so the loop always terminates.
                for _ in range(MAX_DEGRADE_ROUNDS):
                    try:
                        result = _stream_once(
                            session,
                            endpoint,
                            body,
                            connect_timeout=connect_timeout,
                            read_timeout=stream_read_timeout,
                            on_progress=on_progress,
                            on_note=on_note,
                            cancel=cancel,
                        )
                        result.degraded = list(labels)
                        return result
                    except Cancelled:
                        raise
                    except TransientError:
                        # Propagate to the outer loop for backoff-and-retry.
                        raise
                    except LlmHttpError as err:
                        first_error = first_error or err
                        if err.status not in DEGRADABLE_STATUS:
                            raise
                        nxt = degrade_body(body, err, applied)
                        if nxt is None:
                            break
                        body = nxt.body
                        applied.append(nxt.tag)
                        labels.append(nxt.label)
                        note(f"服务拒收了请求参数，{nxt.label}，重试中")
                    except StreamUnsupported as err:
                        first_error = first_error or err
                        break
                # Inner loop finished (break or exhausted) without success.
                # Fall through to non-stream fallback.
                break
            except TransientError as err:
                first_error = first_error or err
                if overload_attempt >= MAX_OVERLOAD_RETRIES:
                    raise
                wait = min(RETRY_AFTER_CAP_S, OVERLOAD_BACKOFF_BASE_S * (2 ** overload_attempt))
                note(
                    f"服务过载，{wait:.0f}s 后重试"
                    f"（第 {overload_attempt + 1}/{MAX_OVERLOAD_RETRIES} 次）"
                )
                _sleep_cancellable(wait, cancel)

        # Non-stream fallback. Deliberately no further degradation here: if this
        # also fails the most useful information is already in first_error.
        note("退回非流式请求重试")
        try:
            body = _replay_degradations(
                applied,
                build_body(
                    config, messages, max_tokens, api_style=endpoint.api_style, stream=False
                ),
            )
            result = _complete_once(
                session,
                endpoint,
                body,
                connect_timeout=connect_timeout,
                read_timeout=total_read_timeout,
                on_note=on_note,
                cancel=cancel,
            )
            result.degraded = [*labels, "退回非流式"]
            return result
        except Cancelled:
            raise
        except Exception as err:
            if first_error is not None:
                raise RuntimeError(
                    f"{first_error}\n（已尝试非流式回退，同样失败：{err}）"
                ) from first_error
            raise
    finally:
        if owned:
            session.close()


def check_connection(
    config: LlmConfigLike,
    *,
    connect_timeout: float = DEFAULT_CONNECT_TIMEOUT_S,
    read_timeout: float = 30.0,
) -> str:
    """探活 + 自检，返回一行人类可读的结论。

    这是给"请求失败但看不出为什么"准备的：一次点击就能分清
    地址错 / Key 错 / 模型名错 / 网络不通，不必再拿一次完整生成去试。
    """
    endpoint = resolve_endpoint(config)
    model = str(getattr(config, "model", "") or "")
    lines = [f"接口风格：{endpoint.api_style}", f"实际请求地址：{endpoint.url}"]

    session = requests.Session()
    try:
        models_url = resolve_models_url(getattr(config, "base_url", ""), endpoint.api_style)
        try:
            response = session.get(
                models_url,
                headers={k: v for k, v in endpoint.headers.items() if k != "Content-Type"},
                timeout=(connect_timeout, read_timeout),
            )
            if response.status_code == 200:
                names = _model_names(response.text)
                lines.append(f"模型列表可读，共 {len(names)} 个")
                if names and model and model not in names:
                    lines.append(
                        f"⚠ 配置的模型 {model} 不在列表里。相近的：" + ", ".join(_closest(model, names))
                    )
            elif response.status_code in {401, 403}:
                lines.append(f"⚠ 鉴权被拒（HTTP {response.status_code}）：API Key 或鉴权方式不对")
            elif response.status_code == 404:
                lines.append("· /models 不可用（很多兼容层没实现），继续用最小生成探活")
            else:
                lines.append(f"· /models 返回 HTTP {response.status_code}")
        except requests.RequestException as err:
            lines.append(f"· /models 探活失败：{err}")

        # 一次最小生成：这才是真正决定"能不能用"的检查。
        try:
            result = generate(
                config,
                [{"role": "user", "content": "ping"}],
                max_tokens=16,
                stream_read_timeout=read_timeout,
                total_read_timeout=read_timeout,
                connect_timeout=connect_timeout,
                session=session,
            )
            lines.append(
                "✓ 生成通路可用"
                + (f"（做了降级：{', '.join(result.degraded)}）" if result.degraded else "")
                + (f"，用量：{result.usage.describe()}" if result.usage.describe() else "")
            )
        except Exception as err:
            lines.append(f"✗ 生成通路不可用：{err}")
    finally:
        session.close()

    return "\n".join(lines)


def _model_names(raw: str) -> list[str]:
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        return []
    items = payload.get("data") if isinstance(payload, dict) else None
    if not isinstance(items, list):
        items = payload.get("models") if isinstance(payload, dict) else None
    if not isinstance(items, list):
        return []
    names: list[str] = []
    for item in items:
        if isinstance(item, dict):
            name = item.get("id") or item.get("name")
            if isinstance(name, str):
                names.append(name)
        elif isinstance(item, str):
            names.append(item)
    return names


def _closest(model: str, names: list[str], limit: int = 3) -> list[str]:
    import difflib

    return difflib.get_close_matches(model, names, n=limit, cutoff=0.3) or names[:limit]

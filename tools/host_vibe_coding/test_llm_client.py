#!/usr/bin/env python3
"""llm_client 的测试。

重点覆盖那些"只有真实服务才会教给你"的分支——它们正是之前请求失败的原因，
而且一旦回归就很难从界面上的报错看出来：

- Base URL 各种填法的归一（路径重复 -> 404）
- 400 之后按 body 精确改字段，而不是无脑退回非流式
- 失败时保留**最初**那条错误
- SSE 里的 error 帧要当错误，不能报成"返回为空"
- Anthropic 的连续 user 合并、system 抽离、事件形状
- 截断（finish=length）必须判失败

用 http.server 起一个假 provider，比 mock 更接近真实：
SSE 分帧、chunked 传输、非 2xx 带 body 这些都是真的走了一遍网络。
"""

from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable

import llm_client as lc

# ---------------------------------------------------------------------------
# 假 provider
# ---------------------------------------------------------------------------


class FakeProvider:
    """一个可编程的本地 HTTP 服务。

    handler 收到 (path, body) 返回 (status, headers, body_bytes)。
    """

    def __init__(self, handler: Callable[[str, dict[str, Any]], tuple[int, dict[str, str], bytes]]):
        self.handler = handler
        self.requests: list[dict[str, Any]] = []
        self.headers_seen: list[dict[str, str]] = []
        self._lock = threading.Lock()
        outer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(length)
                try:
                    body = json.loads(raw)
                except json.JSONDecodeError:
                    body = {}
                with outer._lock:
                    outer.requests.append(body)
                    outer.headers_seen.append({k.lower(): v for k, v in self.headers.items()})
                status, headers, payload = outer.handler(self.path, body)
                self.send_response(status)
                for name, value in headers.items():
                    self.send_header(name, value)
                # 显式关闭连接，避免 handler 线程在响应后继续阻塞在 readinto 等下一个请求：
                # keep-alive 下 serve_forever 收尾时会被这些线程卡住。
                self.send_header("Connection", "close")
                self.close_connection = True
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self) -> None:  # noqa: N802
                status, headers, payload = outer.handler(self.path, {})
                self.send_response(status)
                for name, value in headers.items():
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_: Any) -> None:
                pass

        # 必须是 threading 版：HTTP/1.1 keep-alive 下，降级重试和非流式回退
        # 会需要第二条连接（第一条还没被复用），单线程 server 会直接死锁。
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> "FakeProvider":
        self.thread.start()
        return self

    def __exit__(self, *_: Any) -> None:
        self.server.shutdown()
        self.server.server_close()

    @property
    def base_url(self) -> str:
        host, port = self.server.server_address[:2]
        return f"http://{host}:{port}/v1"


class Config:
    """最小的 config，只带 llm_client 需要的字段。"""

    def __init__(self, base_url: str, **kwargs: Any) -> None:
        self.base_url = base_url
        self.model = kwargs.get("model", "test-model")
        self.api_key = kwargs.get("api_key", "sk-test")
        self.auth_type = kwargs.get("auth_type", "bearer")
        self.max_tokens = kwargs.get("max_tokens", 1000)
        self.api_style = kwargs.get("api_style", lc.API_STYLE_OPENAI_CHAT)
        self.extra_headers = kwargs.get("extra_headers", {})
        self.extra_params = kwargs.get("extra_params", {})


def sse(*frames: str) -> bytes:
    return "".join(f"data: {frame}\n\n" for frame in frames).encode("utf-8")


def openai_chunk(text: str = "", finish: str | None = None, **extra: Any) -> str:
    delta: dict[str, Any] = {}
    if text:
        delta["content"] = text
    choice: dict[str, Any] = {"delta": delta}
    if finish:
        choice["finish_reason"] = finish
    return json.dumps({"choices": [choice], **extra})


def run(config: Config, **kwargs: Any) -> lc.LlmResult:
    return lc.generate(
        config,
        [{"role": "user", "content": "hi"}],
        max_tokens=100,
        stream_read_timeout=10.0,
        total_read_timeout=10.0,
        connect_timeout=5.0,
        **kwargs,
    )


# ---------------------------------------------------------------------------
# endpoint 归一
# ---------------------------------------------------------------------------


def test_endpoint_url_handles_every_base_url_shape():
    """Base URL 填法不统一是 404 的头号原因，而报错只说"请求失败"。"""
    resolve = lc.resolve_endpoint_url
    chat = lc.API_STYLE_OPENAI_CHAT

    # 常规：带 /v1，追加路径
    assert resolve("https://api.deepseek.com/v1", chat) == "https://api.deepseek.com/v1/chat/completions"
    # 末尾斜杠
    assert resolve("https://api.deepseek.com/v1/", chat) == "https://api.deepseek.com/v1/chat/completions"
    # 用户粘了完整 endpoint：绝不能拼成 /chat/completions/chat/completions
    full = "https://api.deepseek.com/v1/chat/completions"
    assert resolve(full, chat) == full
    # 少了 scheme
    assert resolve("api.openai.com/v1", chat) == "https://api.openai.com/v1/chat/completions"
    # 网关的自定义路径
    assert resolve("https://gw.example.com/proxy/openai/v1", chat) == (
        "https://gw.example.com/proxy/openai/v1/chat/completions"
    )
    # 末尾 # = 锁定这个地址，别猜
    assert resolve("https://gw.example.com/weird/path#", chat) == "https://gw.example.com/weird/path"


def test_anthropic_endpoint_never_doubles_the_v1_segment():
    """base_url 带 /v1 时不能拼成 /v1/v1/messages。"""
    resolve = lc.resolve_endpoint_url
    anthropic = lc.API_STYLE_ANTHROPIC

    assert resolve("https://api.anthropic.com/v1", anthropic) == "https://api.anthropic.com/v1/messages"
    assert resolve("https://api.anthropic.com", anthropic) == "https://api.anthropic.com/v1/messages"
    full = "https://api.anthropic.com/v1/messages"
    assert resolve(full, anthropic) == full
    # 代理把 anthropic 挂在别的前缀下
    assert resolve("https://gw.example.com/claude/messages", anthropic) == (
        "https://gw.example.com/claude/messages"
    )


def test_empty_base_url_says_what_to_fix():
    try:
        lc.resolve_endpoint_url("", lc.API_STYLE_OPENAI_CHAT)
    except RuntimeError as err:
        assert "Base URL" in str(err)
    else:
        raise AssertionError("空 base_url 必须报错")


def test_auth_header_follows_api_style():
    """Anthropic 用 x-api-key，OpenAI 系用 Bearer。搞错就是 401。"""
    openai = lc.resolve_endpoint(Config("https://x/v1"))
    assert openai.headers["Authorization"] == "Bearer sk-test"
    assert "x-api-key" not in openai.headers

    claude = lc.resolve_endpoint(Config("https://x/v1", api_style=lc.API_STYLE_ANTHROPIC))
    assert claude.headers["x-api-key"] == "sk-test"
    assert claude.headers["anthropic-version"] == lc.ANTHROPIC_VERSION
    assert "Authorization" not in claude.headers

    # 本地 vLLM / Ollama：不带鉴权
    local = lc.resolve_endpoint(Config("http://127.0.0.1:8000/v1", auth_type="none"))
    assert "Authorization" not in local.headers


# ---------------------------------------------------------------------------
# 请求体
# ---------------------------------------------------------------------------


def test_anthropic_body_splits_system_and_merges_adjacent_user():
    """上下文引擎按段落产出 user 消息，Anthropic 不接受连续两条 user。

    这是"换成 Claude 就全失败"最常见的根因，而报错只说 400。
    """
    messages = [
        {"role": "system", "content": "你是生成器"},
        {"role": "user", "content": "第一段"},
        {"role": "user", "content": "第二段"},
        {"role": "assistant", "content": "好"},
        {"role": "user", "content": "第三段"},
    ]
    body = lc.build_body(
        Config("https://x/v1"), messages, 500, api_style=lc.API_STYLE_ANTHROPIC, stream=True
    )

    assert body["system"] == "你是生成器"
    assert [m["role"] for m in body["messages"]] == ["user", "assistant", "user"]
    assert body["messages"][0]["content"] == "第一段\n\n第二段"
    # max_tokens 在 Anthropic 是必填字段
    assert body["max_tokens"] == 500
    assert "stream_options" not in body, "Anthropic 没有这个字段"


def test_openai_body_keeps_system_in_messages():
    messages = [{"role": "system", "content": "sys"}, {"role": "user", "content": "hi"}]
    body = lc.build_body(
        Config("https://x/v1"), messages, 500, api_style=lc.API_STYLE_OPENAI_CHAT, stream=True
    )
    assert body["messages"] == messages
    assert body["stream_options"] == {"include_usage": True}


def test_extra_params_cannot_hijack_semantic_fields():
    """用户可以加 temperature，但不能顶掉 model/messages/max_tokens。"""
    config = Config("https://x/v1", extra_params={"temperature": 0.2})
    body = lc.build_body(config, [], 500, api_style=lc.API_STYLE_OPENAI_CHAT, stream=False)
    assert body["temperature"] == 0.2

    for name in ("model", "messages", "max_tokens", "stream"):
        try:
            lc.build_body(
                Config("https://x/v1", extra_params={name: "x"}),
                [],
                500,
                api_style=lc.API_STYLE_OPENAI_CHAT,
                stream=False,
            )
        except RuntimeError as err:
            assert name in str(err)
        else:
            raise AssertionError(f"extra_params 不该允许覆盖 {name}")


# ---------------------------------------------------------------------------
# 降级
# ---------------------------------------------------------------------------


def test_degrade_renames_max_tokens_for_new_openai_models():
    """o 系列 / gpt-5 只认 max_completion_tokens。

    原来的实现遇到这个 400 会退回非流式，用同样的字段再发一次、再失败一次，
    然后把第二次的错误报给用户。
    """
    body = {"model": "o3", "max_tokens": 100, "messages": []}
    err = lc.LlmHttpError(
        "OpenAI",
        400,
        json.dumps({"error": {"message": "Unsupported parameter: 'max_tokens' is not supported "
                                         "with this model. Use 'max_completion_tokens' instead."}}),
        "https://x",
    )

    step = lc.degrade_body(body, err, [])
    assert step is not None
    assert step.tag == "max_completion_tokens"
    assert step.body["max_completion_tokens"] == 100
    assert "max_tokens" not in step.body
    # 原 body 不能被就地改坏
    assert body["max_tokens"] == 100


def test_degrade_drops_stream_options_for_local_servers():
    """vLLM / Ollama 不认 stream_options。"""
    body = {"model": "m", "messages": [], "stream": True, "stream_options": {"include_usage": True}}
    err = lc.LlmHttpError("x", 400, "unknown field: stream_options", "https://x")

    step = lc.degrade_body(body, err, [])
    assert step is not None and step.tag == "stream_options"
    assert "stream_options" not in step.body


def test_degrade_never_repeats_the_same_change():
    """已经试过的降级不能再试，否则会转圈。"""
    body = {"model": "m", "messages": [], "stream": True}
    err = lc.LlmHttpError("x", 400, "invalid request", "https://x")
    assert lc.degrade_body(body, err, ["stream_options"]) is None


def test_degrade_never_touches_semantic_fields():
    body = {"model": "m", "messages": [], "stream": True}
    err = lc.LlmHttpError("x", 400, "unsupported parameter 'model'", "https://x")
    step = lc.degrade_body(body, err, [])
    assert step is None or "model" in step.body


# ---------------------------------------------------------------------------
# SSE 解析
# ---------------------------------------------------------------------------


class FakeResponse:
    def __init__(self, chunks: list[bytes]) -> None:
        self.chunks = chunks

    def iter_content(self, chunk_size: int | None = None) -> Any:
        del chunk_size
        return iter(self.chunks)

    def close(self) -> None:
        pass


def test_sse_reassembles_frames_split_across_chunks():
    """路标可能跨 chunk：按 chunk 边界解析会把 JSON 切坏。"""
    raw = b'data: {"a":1}\n\ndata: {"b":2}\n\n'
    # 故意在 JSON 中间切开
    chunks = [raw[:10], raw[10:20], raw[20:]]
    events = [data for _, data in lc.iter_sse(FakeResponse(chunks), None) if data]
    assert events == ['{"a":1}', '{"b":2}']


def test_sse_reports_keepalive_frames_as_alive_but_empty():
    """": ping" 没有内容，但足以说明连接是活的——静默超时不该被它触发。"""
    events = list(lc.iter_sse(FakeResponse([b": ping\n\n", b'data: {"x":1}\n\n']), None))
    assert ("", "") in events
    assert any(data == '{"x":1}' for _, data in events)


def test_sse_carries_the_event_name_for_anthropic():
    raw = b'event: content_block_delta\ndata: {"type":"content_block_delta"}\n\n'
    events = [(name, data) for name, data in lc.iter_sse(FakeResponse([raw]), None) if data]
    assert events[0][0] == "content_block_delta"


def test_sse_is_cancellable_mid_stream():
    cancel = threading.Event()
    cancel.set()
    try:
        list(lc.iter_sse(FakeResponse([b'data: {"x":1}\n\n']), cancel))
    except lc.Cancelled:
        return
    raise AssertionError("取消令牌已置位，iter_sse 必须抛 Cancelled")


# ---------------------------------------------------------------------------
# 流里的错误帧
# ---------------------------------------------------------------------------


def test_error_frames_are_recognized_in_every_known_shape():
    """HTTP 200 之后服务照样会推 error 帧，藏的位置有好几种。

    之前这些帧被忽略，最后报"LLM 返回为空"，真正的原因全丢了。
    """
    cases = [
        {"type": "error", "message": "内部错误"},
        {"error": {"message": "quota exceeded"}},
        {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}},
        {"type": "response.failed", "response": {"error": {"message": "generation failed"}}},
    ]
    for value in cases:
        problem = lc.provider_event_error("测试", value)
        assert problem, f"没认出错误帧：{value}"

    # 正常帧不能误判
    assert lc.provider_event_error("测试", {"type": "response.completed", "error": None}) is None
    assert lc.provider_event_error("测试", {"choices": [{"delta": {"content": "hi"}}]}) is None


# ---------------------------------------------------------------------------
# finish / usage
# ---------------------------------------------------------------------------


def test_length_always_stays_length():
    """截断的脚本能过语法但秒退，必须当场判失败，不能被降级成 stop。"""
    assert lc.map_openai_finish("length") == "length"
    assert lc.map_openai_finish("max_tokens") == "length"
    assert lc.map_openai_finish("model_context_window_exceeded") == "length"
    assert lc.map_openai_finish("stop") == "stop"
    assert lc.map_openai_finish("content_filter") == "content_filter"
    assert lc.map_openai_finish("") == ""


def test_usage_merge_keeps_the_real_number_when_later_frames_report_zero():
    """流式里同一字段会被报好几次，后一次可能是缺省 0，覆盖会擦掉真实值。"""
    total = lc.Usage()
    lc.merge_usage(total, lc.Usage(input_tokens=414, output_tokens=0))
    lc.merge_usage(total, lc.Usage(output_tokens=191))
    assert total.input_tokens == 414
    assert total.output_tokens == 191
    assert total.total_tokens == 605


def test_anthropic_usage_counts_cached_input_once():
    usage = lc.anthropic_usage(
        {"input_tokens": 10, "output_tokens": 5,
         "cache_read_input_tokens": 20, "cache_creation_input_tokens": 30}
    )
    assert usage is not None
    assert usage.input_tokens == 60
    assert usage.output_tokens == 5
    assert usage.cache_read_tokens == 20


# ---------------------------------------------------------------------------
# Anthropic 事件翻译
# ---------------------------------------------------------------------------


def test_anthropic_events_translate_to_text_thinking_and_stop():
    text = lc.translate_anthropic_event(
        "content_block_delta",
        {"type": "content_block_delta", "delta": {"type": "text_delta", "text": "hello"}},
    )
    assert text.text == "hello" and not text.reasoning

    think = lc.translate_anthropic_event(
        "content_block_delta",
        {"type": "content_block_delta", "delta": {"type": "thinking_delta", "thinking": "hmm"}},
    )
    assert think.reasoning == "hmm" and not think.text

    stop = lc.translate_anthropic_event(
        "message_delta", {"type": "message_delta", "delta": {"stop_reason": "max_tokens"}}
    )
    assert stop.finish == "length"

    done = lc.translate_anthropic_event("message_stop", {"type": "message_stop"})
    assert done.done


def test_openai_reasoning_field_is_picked_up_under_both_names():
    for name in ("reasoning_content", "reasoning"):
        delta = lc.translate_openai_chunk({"choices": [{"delta": {name: "思考"}}]})
        assert delta.reasoning == "思考", name


# ---------------------------------------------------------------------------
# 端到端（真的走一遍网络）
# ---------------------------------------------------------------------------


def test_streaming_generate_collects_text_and_usage():
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        assert path == "/v1/chat/completions", path
        assert body["stream"] is True
        return 200, {"Content-Type": "text/event-stream"}, sse(
            openai_chunk("local "),
            openai_chunk("function main"),
            openai_chunk(finish="stop", usage={"prompt_tokens": 12, "completion_tokens": 34}),
            "[DONE]",
        )

    with FakeProvider(handler) as provider:
        pieces: list[str] = []
        result = run(
            Config(provider.base_url),
            on_progress=lambda piece, is_reasoning: pieces.append(piece) if piece else None,
        )

    assert result.text == "local function main"
    assert result.finish == "stop"
    assert result.usage.output_tokens == 34
    assert pieces == ["local ", "function main"], "进度回调必须逐块回报"


def test_400_on_max_tokens_is_fixed_by_renaming_not_by_giving_up():
    """完整走一遍：第一次 400 点名 max_tokens，改字段后第二次成功。"""
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path
        if "max_tokens" in body:
            return 400, {"Content-Type": "application/json"}, json.dumps(
                {"error": {"message": "Unsupported parameter: 'max_tokens'. "
                                      "Use 'max_completion_tokens' instead."}}
            ).encode()
        assert "max_completion_tokens" in body
        return 200, {"Content-Type": "text/event-stream"}, sse(
            openai_chunk("ok"), openai_chunk(finish="stop"), "[DONE]"
        )

    with FakeProvider(handler) as provider:
        notes: list[str] = []
        result = run(Config(provider.base_url), on_note=notes.append)
        assert len(provider.requests) == 2, "应该只多发一次，不该转圈"

    assert result.text == "ok"
    assert any("max_completion_tokens" in note for note in notes)
    assert result.degraded, "降级必须回报给用户，否则配置里的问题永远不会被修"


def test_auth_failure_is_not_retried_and_keeps_the_server_message():
    """401 改字段治不好。重试只是白烧一次请求，还会盖掉真正的原因。"""
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        return 401, {"Content-Type": "application/json"}, json.dumps(
            {"error": {"message": "Incorrect API key provided"}}
        ).encode()

    with FakeProvider(handler) as provider:
        try:
            run(Config(provider.base_url))
        except RuntimeError as err:
            assert "401" in str(err)
            assert "Incorrect API key" in str(err), "服务的原话必须带出来"
        else:
            raise AssertionError("401 必须失败")
        assert len(provider.requests) == 1, "401 不该重试"


def test_first_error_survives_a_failed_non_stream_fallback():
    """这是原来最难排查的一点：回退失败后报的是第二次的错误。

    用户看到"返回为空"，真实原因是"模型名不存在"。
    """
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path
        message = "model 'nope' not found" if body.get("stream") else "still broken"
        return 400, {"Content-Type": "application/json"}, json.dumps(
            {"error": {"message": message}}
        ).encode()

    with FakeProvider(handler) as provider:
        try:
            run(Config(provider.base_url, model="nope"))
        except RuntimeError as err:
            assert "not found" in str(err), f"最初的原因丢了：{err}"
        else:
            raise AssertionError("必须失败")


def test_error_frame_mid_stream_beats_reporting_an_empty_reply():
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        return 200, {"Content-Type": "text/event-stream"}, sse(
            openai_chunk("部分内容"),
            json.dumps({"error": {"message": "upstream timeout"}}),
        )

    with FakeProvider(handler) as provider:
        try:
            run(Config(provider.base_url))
        except RuntimeError as err:
            assert "upstream timeout" in str(err)
            assert "返回为空" not in str(err)
        else:
            raise AssertionError("流中的 error 帧必须当失败")


def test_rate_limit_is_retried_and_then_succeeds():
    state = {"hits": 0}

    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        state["hits"] += 1
        if state["hits"] == 1:
            return 429, {"Retry-After": "0"}, b'{"error":{"message":"slow down"}}'
        return 200, {"Content-Type": "text/event-stream"}, sse(
            openai_chunk("ok"), openai_chunk(finish="stop"), "[DONE]"
        )

    with FakeProvider(handler) as provider:
        notes: list[str] = []
        result = run(Config(provider.base_url), on_note=notes.append)

    assert result.text == "ok"
    assert state["hits"] == 2
    assert any("429" in note for note in notes), "重试要让用户看见，否则界面像卡住"


def test_anthropic_end_to_end_uses_native_messages_shape():
    """原生 /v1/messages 全链路：请求头、system 抽离、事件解析、usage。"""
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        assert path == "/v1/messages", path
        assert body["system"] == "你是生成器"
        assert body["max_tokens"] == 100
        return 200, {"Content-Type": "text/event-stream"}, (
            b'event: message_start\ndata: {"type":"message_start","message":'
            b'{"usage":{"input_tokens":50,"output_tokens":0}}}\n\n'
            b'event: content_block_delta\ndata: {"type":"content_block_delta",'
            b'"delta":{"type":"text_delta","text":"print(1)"}}\n\n'
            b'event: message_delta\ndata: {"type":"message_delta",'
            b'"delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":9}}\n\n'
            b'event: message_stop\ndata: {"type":"message_stop"}\n\n'
        )

    config = Config(
        FakeProvider(lambda p, b: (200, {}, b"")).base_url,  # 占位，下面替换
        api_style=lc.API_STYLE_ANTHROPIC,
        model="claude-sonnet-5",
    )
    with FakeProvider(handler) as provider:
        config.base_url = provider.base_url
        result = lc.generate(
            config,
            [{"role": "system", "content": "你是生成器"}, {"role": "user", "content": "hi"}],
            max_tokens=100,
            stream_read_timeout=10.0,
            total_read_timeout=10.0,
        )
        headers = provider.headers_seen[-1]

    assert result.text == "print(1)"
    assert result.finish == "stop"
    assert result.usage.input_tokens == 50
    assert result.usage.output_tokens == 9
    assert headers["x-api-key"] == "sk-test"
    assert headers["anthropic-version"] == lc.ANTHROPIC_VERSION
    assert "authorization" not in headers, "Anthropic 不该带 Bearer 头"


def test_non_sse_response_falls_back_to_plain_completion():
    """有的服务对 stream=true 直接回一个普通 JSON。"""
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path
        if body.get("stream"):
            return 200, {"Content-Type": "application/json"}, b'{"id":"x","object":"chat.completion"}'
        return 200, {"Content-Type": "application/json"}, json.dumps(
            {"choices": [{"message": {"content": "fallback ok"}, "finish_reason": "stop"}]}
        ).encode()

    with FakeProvider(handler) as provider:
        result = run(Config(provider.base_url))

    assert result.text == "fallback ok"
    assert result.streamed is False


def test_cancel_stops_before_sending():
    cancel = threading.Event()
    cancel.set()

    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        raise AssertionError("取消后不该发出请求")

    with FakeProvider(handler) as provider:
        try:
            run(Config(provider.base_url), cancel=cancel)
        except lc.Cancelled:
            pass
        else:
            raise AssertionError("必须抛 Cancelled")
        assert not provider.requests


def test_extra_headers_reach_the_wire():
    """自建网关常要 x-tenant 之类的头。"""
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        return 200, {"Content-Type": "text/event-stream"}, sse(
            openai_chunk("ok"), openai_chunk(finish="stop"), "[DONE]"
        )

    with FakeProvider(handler) as provider:
        run(Config(provider.base_url, extra_headers={"X-Tenant": "acme"}))
        assert provider.headers_seen[-1]["x-tenant"] == "acme"


def test_overload_error_in_sse_stream_is_retried():
    """Sub2API 这类网关过载时在 SSE 流里推 {"error": "overloaded"} 而不是 HTTP 503。

    原来这条错误被当成普通 RuntimeError 抛出，不触发重试。
    现在应该自动等待退避后重试，直到成功。
    """
    state = {"hits": 0}

    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        state["hits"] += 1
        if state["hits"] < 3:
            # 服务端在 200 的 SSE 流里推过载错误帧
            return 200, {"Content-Type": "text/event-stream"}, sse(
                json.dumps({"error": {"message": "Our servers are currently overloaded. Please try again later."}}),
            )
        return 200, {"Content-Type": "text/event-stream"}, sse(
            openai_chunk("recovered"), openai_chunk(finish="stop"), "[DONE]"
        )

    with FakeProvider(handler) as provider:
        notes: list[str] = []
        # 把退避时间缩短，否则测试要等真实的 3s+
        orig_base = lc.OVERLOAD_BACKOFF_BASE_S
        lc.OVERLOAD_BACKOFF_BASE_S = 0.05
        try:
            result = run(Config(provider.base_url), on_note=notes.append)
        finally:
            lc.OVERLOAD_BACKOFF_BASE_S = orig_base

    assert result.text == "recovered", f"期望成功，实际：{result.text!r}"
    assert state["hits"] == 3, f"期望重试 2 次，实际发了 {state['hits']} 次请求"
    assert any("过载" in note for note in notes), "过载重试必须通知用户，否则界面像卡住"


def test_overload_error_gives_up_after_max_retries():
    """重试次数耗尽后应该抛出原始过载错误，不能无限等待。"""
    def handler(path: str, body: dict) -> tuple[int, dict[str, str], bytes]:
        del path, body
        return 200, {"Content-Type": "text/event-stream"}, sse(
            json.dumps({"error": {"message": "overloaded"}}),
        )

    with FakeProvider(handler) as provider:
        orig_base = lc.OVERLOAD_BACKOFF_BASE_S
        orig_max = lc.MAX_OVERLOAD_RETRIES
        lc.OVERLOAD_BACKOFF_BASE_S = 0.01
        lc.MAX_OVERLOAD_RETRIES = 2
        try:
            run(Config(provider.base_url))
        except RuntimeError as err:
            assert "overloaded" in str(err).lower(), f"错误信息里应该有 overloaded：{err}"
            assert provider.requests, "至少应该发了一次请求"
        except Exception as err:
            raise AssertionError(f"期望 RuntimeError，得到 {type(err).__name__}: {err}") from err
        else:
            raise AssertionError("过载持续时必须最终抛出错误")
        finally:
            lc.OVERLOAD_BACKOFF_BASE_S = orig_base
            lc.MAX_OVERLOAD_RETRIES = orig_max


def main() -> None:
    """不装 pytest 也能跑。"""
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print(f"  ok  {name}")
        except Exception as err:  # noqa: BLE001
            failed += 1
            print(f"FAIL  {name}: {err}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()

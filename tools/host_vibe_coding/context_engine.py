#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""上位机侧的上下文组织引擎。

结构照搬设备端 claw_core 的 context provider 模型
（components/claw_modules/claw_core/src/claw_core_context.c）：

* 每段上下文是一个具名 provider，声明自己产出 SYSTEM 段还是 MESSAGES 段；
  provider 返回 None 等价于设备端的 ESP_ERR_NOT_FOUND —— 这一轮不注入。
* provider 分 stable / dynamic 两类。stable 对标设备端的
  CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY：整个任务只 collect 一次并缓存，
  后续重试轮直接复用，保证 prompt 前缀逐字节稳定，能命中服务端 prompt cache。
* 组装完记录每个 provider 实际注入了多少字符（对标 context_providers_csv 与
  "context_cached ... context_len=%u" 日志），失败时能直接看出是哪段没进去。

与设备端的区别：上位机没有 tools 通道（生成 Lua 走纯文本），所以只有 SYSTEM 和
MESSAGES 两种 kind；另外多一层字符预算裁剪，因为 README 体量比设备端的 SKILL.md 大得多。
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from enum import Enum
from typing import Callable, Iterable

#: 整个 system prompt 的字符预算。留给云端模型的上下文窗口通常远大于此，
#: 但把 API 文档无节制堆进去会稀释关键约束，实测反而更容易生成幻觉 API。
DEFAULT_CHAR_BUDGET = 48000


class ContextKind(str, Enum):
    """对标 claw_core_context_kind_t，去掉上位机用不到的 TOOLS。"""

    SYSTEM = "system_prompt"
    MESSAGES = "messages"


@dataclass(frozen=True)
class ContextSection:
    """一个 provider 的产出。

    name     段名，进 system prompt 的小节标题，也是 trace 里的标识
    kind     进 system 段还是进 messages 数组
    content  正文；SYSTEM 用字符串，MESSAGES 用 [{"role":..,"content":..}]
    priority 数字越小越重要。预算不足时从最大的开始裁。
    """

    name: str
    kind: ContextKind
    content: str | list[dict[str, str]]
    priority: int = 50

    @property
    def size(self) -> int:
        if isinstance(self.content, str):
            return len(self.content)
        return sum(len(str(m.get("content", ""))) for m in self.content)

    def truncated(self, budget: int) -> "ContextSection | None":
        """按预算裁剪。MESSAGES 段不做部分裁剪——截断对话会产生误导性上下文，宁可整段丢弃。"""
        if budget <= 0:
            return None
        if self.kind is ContextKind.MESSAGES:
            return self if self.size <= budget else None
        if self.size <= budget:
            return self
        text = str(self.content)[:budget].rstrip()
        return ContextSection(
            name=self.name,
            kind=self.kind,
            content=f"{text}\n...[本段因预算截断，未显示的内容请勿凭猜测使用]",
            priority=self.priority,
        )


@dataclass
class ContextRequest:
    """对标 claw_core_request_t：一次生成请求的全部输入。"""

    prompt: str
    device_info: str = ""
    history: list[dict[str, object]] = field(default_factory=list)
    plan: object | None = None
    attempts: list[object] = field(default_factory=list)
    round_index: int = 1
    #: stable provider 的缓存键。同一个任务的多轮重试要用同一个键。
    session_key: str = ""

    @property
    def is_first_round(self) -> bool:
        return self.round_index <= 1

    @property
    def cache_scope(self) -> str:
        """stable 段的实际缓存作用域。

        显式给了 session_key 就用它；没给时退回 prompt 的哈希，而不是共用空串——
        否则不同任务会命中同一个槽，第二个任务拿到第一个任务的 API 文档。
        同一任务重试时 prompt 不变，缓存照旧命中。
        """
        if self.session_key:
            return self.session_key
        return hashlib.sha1(self.prompt.encode("utf-8")).hexdigest()[:16]


CollectFn = Callable[[ContextRequest], ContextSection | None]


@dataclass(frozen=True)
class ContextProvider:
    """对标 claw_core_context_provider_t。"""

    name: str
    collect: CollectFn
    #: True 等价于设备端的 REQUEST_START_ONLY：只在首轮 collect，之后复用缓存。
    stable: bool = False


@dataclass(frozen=True)
class ProviderTrace:
    """单个 provider 的组装结果，用于诊断。"""

    name: str
    kind: str
    size: int
    stable: bool
    cached: bool
    dropped: bool = False
    truncated: bool = False
    error: str = ""

    def describe(self) -> str:
        if self.error:
            return f"{self.name}: 出错 ({self.error})"
        flags = []
        if self.cached:
            flags.append("缓存复用")
        if self.truncated:
            flags.append("已截断")
        if self.dropped:
            flags.append("超预算丢弃")
        suffix = f" [{'/'.join(flags)}]" if flags else ""
        return f"{self.name}: {self.size} 字符{suffix}"


@dataclass(frozen=True)
class BuiltContext:
    """组装完成的一次请求上下文。"""

    messages: list[dict[str, str]]
    trace: list[ProviderTrace]

    @property
    def total_chars(self) -> int:
        return sum(len(str(m.get("content", ""))) for m in self.messages)

    @property
    def providers_csv(self) -> str:
        """对标设备端 completion summary 里的 context_providers_csv。"""
        return ",".join(t.name for t in self.trace if t.size > 0 and not t.dropped)

    def describe(self) -> str:
        lines = [f"上下文组装：{len(self.messages)} 条消息，共 {self.total_chars} 字符"]
        lines.extend(f"  - {t.describe()}" for t in self.trace)
        return "\n".join(lines)

    def section(self, name: str) -> ProviderTrace | None:
        for t in self.trace:
            if t.name == name:
                return t
        return None


def format_prompt_section(name: str, content: str) -> str:
    """对标 claw_core_context.c 的 append_prompt_section()：每段带具名标题。"""
    return f"## {name}\n\n{content.strip()}"


class ContextBuilder:
    """按注册顺序收集 provider，产出最终 messages。

    注册顺序即 prompt 里的出现顺序，调用方要把稳定段排在动态段之前，
    否则 stable 缓存拿不到 prompt 前缀稳定的好处。
    """

    def __init__(self, *, char_budget: int = DEFAULT_CHAR_BUDGET) -> None:
        self._providers: list[ContextProvider] = []
        self._char_budget = char_budget
        #: (cache_scope, provider name) -> 首轮 collect 的结果
        self._stable_cache: dict[tuple[str, str], ContextSection | None] = {}

    def register(self, provider: ContextProvider) -> "ContextBuilder":
        if any(p.name == provider.name for p in self._providers):
            raise ValueError(f"provider 名字重复：{provider.name}")
        self._providers.append(provider)
        return self

    def register_all(self, providers: Iterable[ContextProvider]) -> "ContextBuilder":
        for provider in providers:
            self.register(provider)
        return self

    def invalidate(self, session_key: str = "") -> None:
        """任务结束或设备信息变化时清掉稳定段缓存。

        参数是 ContextRequest.cache_scope 的值：显式 session_key，或不给
        session_key 时的 prompt 哈希。不传则全清。
        """
        if not session_key:
            self._stable_cache.clear()
            return
        for key in [k for k in self._stable_cache if k[0] == session_key]:
            del self._stable_cache[key]

    def _collect_one(
        self,
        provider: ContextProvider,
        request: ContextRequest,
    ) -> tuple[ContextSection | None, bool, str]:
        """返回 (section, 是否命中缓存, 错误信息)。"""
        cache_key = (request.cache_scope, provider.name)
        if provider.stable and cache_key in self._stable_cache:
            return self._stable_cache[cache_key], True, ""

        try:
            section = provider.collect(request)
        except Exception as err:  # provider 出错不该让整次生成失败
            return None, False, f"{type(err).__name__}: {err}"

        if section is not None and section.size == 0:
            section = None
        if provider.stable:
            self._stable_cache[cache_key] = section
        return section, False, ""

    def build(self, request: ContextRequest) -> BuiltContext:
        collected: list[tuple[ContextProvider, ContextSection | None, bool, str]] = []
        for provider in self._providers:
            section, cached, error = self._collect_one(provider, request)
            collected.append((provider, section, cached, error))

        kept = self._apply_budget([(p, s) for p, s, _, _ in collected if s is not None])

        system_parts: list[str] = []
        message_parts: list[dict[str, str]] = []
        for provider, section in kept:
            if section.kind is ContextKind.SYSTEM:
                system_parts.append(format_prompt_section(section.name, str(section.content)))
            else:
                message_parts.extend(section.content)  # type: ignore[arg-type]

        messages: list[dict[str, str]] = []
        if system_parts:
            messages.append({"role": "system", "content": "\n\n".join(system_parts)})
        messages.extend(message_parts)
        messages.append({"role": "user", "content": request.prompt})

        kept_by_name = {p.name: s for p, s in kept}
        trace: list[ProviderTrace] = []
        for provider, section, cached, error in collected:
            final = kept_by_name.get(provider.name)
            trace.append(
                ProviderTrace(
                    name=provider.name,
                    kind=section.kind.value if section else "-",
                    size=final.size if final else 0,
                    stable=provider.stable,
                    cached=cached,
                    dropped=section is not None and final is None,
                    truncated=bool(section and final and final.size < section.size),
                    error=error,
                )
            )

        return BuiltContext(messages=messages, trace=trace)

    def _apply_budget(
        self,
        sections: list[tuple[ContextProvider, ContextSection]],
    ) -> list[tuple[ContextProvider, ContextSection]]:
        """预算不足时按 priority 从大到小裁剪，保持注册顺序返回。"""
        total = sum(s.size for _, s in sections)
        if total <= self._char_budget:
            return sections

        order = sorted(range(len(sections)), key=lambda i: -sections[i][1].priority)
        result: dict[int, ContextSection] = {i: s for i, (_, s) in enumerate(sections)}
        overflow = total - self._char_budget

        for index in order:
            if overflow <= 0:
                break
            section = result[index]
            allowed = max(0, section.size - overflow)
            shrunk = section.truncated(allowed)
            freed = section.size - (shrunk.size if shrunk else 0)
            overflow -= freed
            if shrunk is None:
                del result[index]
            else:
                result[index] = shrunk

        return [(sections[i][0], result[i]) for i in range(len(sections)) if i in result]

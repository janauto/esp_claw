#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""上位机的 provider 集合：把设备信息、playbook、API 文档、规划、错误历史组织成上下文。

注册顺序即 prompt 顺序，刻意按"稳定 -> 动态"排列：

    system_rules   stable   输出格式与基线约束
    device         stable   设备信息
    playbook       stable   任务类型专属规范（对标 SKILL.md 全文）
    api_reference  stable   选中模块的 README（渐进式披露的第二层）
    plan           stable   规划阶段产出的实现计划
    ----- 以上在整个任务的多轮重试里逐字节不变，能命中 prompt cache -----
    diagnosis      dynamic  上一轮错误的定向诊断
    history        dynamic  跨任务的会话历史
    repair         dynamic  本任务前几轮的脚本 + 真实设备报错

repair 段是修复回路的核心：它把每一轮的脚本原文和设备日志成对回灌，
模型才能"只改出错的那部分"。这是 repair_messages() 一直想做的事。
"""

from __future__ import annotations

import lua_module_catalog as catalog
import playbook_registry
from context_engine import (
    ContextKind,
    ContextProvider,
    ContextRequest,
    ContextSection,
)
from task_planner import TaskPlan

#: 单轮回灌给模型的脚本原文上限。太长会挤掉 API 文档，而修 bug 通常只需要看到全貌一次。
MAX_SCRIPT_ECHO = 4000
#: 单轮设备日志上限。
MAX_LOG_ECHO = 1200
#: 最多回灌几轮修复历史。更早的轮次信息价值低且会稀释当前错误。
MAX_REPAIR_ROUNDS = 3

OUTPUT_CONTRACT = """你为 ESP-Claw 设备生成可直接在设备 Lua runtime 执行的脚本。

**输出格式**：只返回一个 ```lua 代码块，块内是完整可运行的脚本。不要在代码块外写任何解释、说明或步骤描述。不要返回多个代码块。"""


def _plan_of(request: ContextRequest) -> TaskPlan | None:
    plan = request.plan
    return plan if isinstance(plan, TaskPlan) else None


def system_rules_provider(extra: str = "") -> ContextProvider:
    """输出契约 + 生成规则。优先级最高，永不被预算裁掉。"""

    def collect(request: ContextRequest) -> ContextSection | None:
        parts = [OUTPUT_CONTRACT]
        if extra.strip():
            parts.append(extra.strip())
        return ContextSection(
            name="输出要求与生成规则",
            kind=ContextKind.SYSTEM,
            content="\n\n".join(parts),
            priority=1,
        )

    return ContextProvider(name="system_rules", collect=collect, stable=True)


def device_provider() -> ContextProvider:
    def collect(request: ContextRequest) -> ContextSection | None:
        if not request.device_info.strip():
            return None
        return ContextSection(
            name="目标设备",
            kind=ContextKind.SYSTEM,
            content=request.device_info.strip(),
            priority=10,
        )

    return ContextProvider(name="device", collect=collect, stable=True)


def playbook_provider(registry: list[playbook_registry.Playbook]) -> ContextProvider:
    """按规划结果挑 playbook 并注入全文。对标 claw_skill_read_document()。"""

    def collect(request: ContextRequest) -> ContextSection | None:
        if not registry:
            return None
        plan = _plan_of(request)
        if plan is not None:
            task_kind, ui_stack, modules = plan.task_kind, plan.ui_stack, plan.modules
        else:
            #: 规划调用失败时和 api_reference 用同一套兜底，避免两个 provider 降级不一致。
            #: task_kind 无法从关键词可靠推断，留空——只按模块匹配的 playbook 仍能入选。
            modules = catalog.select_by_keywords(request.prompt)
            ui_stack = "lvgl" if "lvgl" in modules else ("display" if "display" in modules else "none")
            task_kind = ""
        picked = playbook_registry.select(
            registry,
            task_kind=task_kind,
            ui_stack=ui_stack,
            modules=modules,
        )
        if not picked:
            return None
        if plan is not None:
            plan.playbooks = [pb.name for pb in picked]
        body = playbook_registry.render(picked)
        if not body:
            return None
        return ContextSection(
            name="实现规范",
            kind=ContextKind.SYSTEM,
            content=body,
            priority=15,
        )

    return ContextProvider(name="playbook", collect=collect, stable=True)


def api_reference_provider() -> ContextProvider:
    """加载选中模块的 README 全文。这是最大的一段，但也是准确性的根本来源。"""

    def collect(request: ContextRequest) -> ContextSection | None:
        plan = _plan_of(request)
        modules = plan.modules if plan else catalog.select_by_keywords(request.prompt)
        if not modules:
            return None
        context, docs = catalog.load_module_docs(modules)
        if not context.strip():
            return None
        if plan is not None:
            plan.docs[:] = docs
        return ContextSection(
            name="模块 API 参考",
            kind=ContextKind.SYSTEM,
            content=context,
            priority=20,
        )

    return ContextProvider(name="api_reference", collect=collect, stable=True)


def plan_provider() -> ContextProvider:
    def collect(request: ContextRequest) -> ContextSection | None:
        plan = _plan_of(request)
        if plan is None:
            return None
        body = plan.as_context()
        if not body.strip():
            return None
        return ContextSection(
            name="实现计划",
            kind=ContextKind.SYSTEM,
            content=body,
            priority=5,
        )

    return ContextProvider(name="plan", collect=collect, stable=True)


def diagnose(summary: str, log: str = "") -> str:
    """把设备端报错翻译成定向修复指引。

    只匹配设备真实输出的英文 Lua 错误文本——之前那套中文正则匹配的是需求描述，
    对设备日志基本不会命中。
    """
    import re

    blob = f"{summary}\n{log}"
    hints: list[str] = []

    nil_call = re.search(r"attempt to call a nil value \((?:field|global|method) '([\w.]+)'\)", blob)
    if nil_call:
        hints.append(
            f"`{nil_call.group(1)}` 不存在。回到上面的 API 参考确认该模块到底有哪些函数，"
            "找不到就说明这个 API 没有，必须换已存在的 API 实现，不要改个名字再猜一次。"
        )

    nil_index = re.search(r"attempt to index a nil value \((?:field|global|local) '(\w+)'\)", blob)
    if nil_index:
        name = nil_index.group(1)
        hints.append(
            f"`{name}` 是 nil。常见原因：模块没 require、句柄获取失败，"
            f"或者把多返回值函数（如 `board_manager.get_display_lcd_params`）当成 table 接收了。"
        )

    if "handle expected" in blob or re.search(r"bad argument #1.*(?:userdata|handle)", blob):
        hints.append(
            "缺少句柄参数。`lcd_touch.*`、`display.init` 这类函数的句柄必须先从 "
            "`board_manager.get_*` 取到再传进去。"
        )

    if re.search(r"number has no integer representation|bad argument.*number expected.*got number|display .* must be an integer", blob):
        hints.append(
            "传了浮点数给要求整数的参数。display 的坐标/宽高/半径/font_size 不会自动取整，"
            "所有除法改用 `//`，或用 `math.floor()` 显式转整。"
        )

    if re.search(r"lvgl style '([^']+)' must be", blob):
        prop = re.search(r"lvgl style '([^']+)' must be", blob).group(1)
        hints.append(
            f"LVGL 样式 `{prop}` 的值类型不对。颜色字段只能传 `0xRRGGBB` 数字或 "
            "`\"#RRGGBB\"` 字符串，不能传 nil/table/布尔值；检查命名色板变量是否拼错，"
            "不要把 display 模块的 `{r=,g=,b=}` 颜色表传给 lvgl。"
        )

    if "lvgl lock failed" in blob or "ESP_ERR_TIMEOUT" in blob:
        hints.append(
            "LVGL 运行时被上一个脚本或系统 UI 占用。下一轮执行前必须先停止旧 job 并等待释放；"
            "脚本自身也要避免在回调里长时间阻塞，主循环每帧只做轻量逻辑并调用 `lvgl.process_events(16)`。"
        )

    bad_arg = re.search(r"bad argument #(\d+) to '(\w+)' \(([^)]+)\)", blob)
    if bad_arg and not hints:
        hints.append(
            f"`{bad_arg.group(2)}` 的第 {bad_arg.group(1)} 个参数不对（{bad_arg.group(3)}）。"
            "回到 API 参考核对完整签名，特别是参数个数。"
        )

    if re.search(r"module '(\w+)' not found", blob):
        missing = re.search(r"module '(\w+)' not found", blob).group(1)
        hints.append(f"模块 `{missing}` 在本设备上不存在，只能 require 计划里列出的模块。")

    if "not enough memory" in blob or "out of memory" in blob:
        hints.append("内存不足。缩小缓冲区、降低分辨率或改成逐行/分块处理，不要一次分配整屏缓冲。")

    if not hints:
        return ""
    return "\n".join(f"- {hint}" for hint in hints)


def diagnosis_provider() -> ContextProvider:
    """针对最近一次失败给出定向诊断。动态段，每轮重收。"""

    def collect(request: ContextRequest) -> ContextSection | None:
        if not request.attempts:
            return None
        for attempt in reversed(request.attempts):
            outcome = getattr(attempt, "outcome", None)
            if outcome is None or getattr(outcome, "ok", False):
                continue
            hints = diagnose(getattr(outcome, "summary", ""), getattr(outcome, "log", ""))
            if hints:
                return ContextSection(
                    name="上一轮失败的定向诊断",
                    kind=ContextKind.SYSTEM,
                    content=hints,
                    priority=2,
                )
            break
        return None

    return ContextProvider(name="diagnosis", collect=collect)


def history_provider(limit: int = 4) -> ContextProvider:
    """跨任务的会话历史。优先级最低，预算紧张时先丢它。"""

    def collect(request: ContextRequest) -> ContextSection | None:
        messages: list[dict[str, str]] = []
        for item in request.history[-limit:]:
            prompt = item.get("prompt")
            result = item.get("result")
            if isinstance(prompt, str) and prompt.strip():
                messages.append({"role": "user", "content": prompt})
            if isinstance(result, str) and result.strip():
                messages.append({"role": "assistant", "content": f"上次执行结果:\n{result[:1500]}"})
        if not messages:
            return None
        return ContextSection(
            name="会话历史",
            kind=ContextKind.MESSAGES,
            content=messages,
            priority=90,
        )

    return ContextProvider(name="history", collect=collect)


def repair_provider() -> ContextProvider:
    """把本任务前几轮的脚本原文和真实报错成对回灌。

    这是让模型能"只改出错那部分"的唯一途径：它必须看到自己上一轮写了什么。
    本地校验失败（attempt.error）和设备端运行失败（attempt.outcome）都要覆盖，
    否则本地校验挂掉的那轮反馈为空，模型会原样再生成一次，直接触发去重退出。
    """

    def collect(request: ContextRequest) -> ContextSection | None:
        attempts = [a for a in request.attempts if getattr(a, "script", "")]
        if not attempts:
            return None

        messages: list[dict[str, str]] = []
        for attempt in attempts[-MAX_REPAIR_ROUNDS:]:
            index = getattr(attempt, "index", 0)
            script = str(getattr(attempt, "script", ""))[:MAX_SCRIPT_ECHO]
            messages.append({"role": "assistant", "content": f"```lua\n{script}\n```"})

            error = str(getattr(attempt, "error", "") or "")
            outcome = getattr(attempt, "outcome", None)

            if error:
                detail = f"第 {index} 轮没有通过上位机本地校验。\n\n**校验错误**：{error}"
            elif getattr(outcome, "flickered", False):
                # 设备端不报错：脚本"正常"跑完退出了。但画面随之销毁，
                # 用户看到的是闪一下就回主界面。必须把这件事讲透，否则模型
                # 只会重新交一份同样没有主循环的代码。
                detail = (
                    f"第 {index} 轮在设备上跑完就退出了"
                    f"（status=done，仅运行 {getattr(outcome, 'runtime_s', 0)}s），**这是失败**。\n\n"
                    "**现象**：屏幕闪一下就回到主界面，设备日志没有任何输出。\n"
                    "**原因**：脚本只创建了 LVGL 对象／定义了状态表，就走到了文件末尾。"
                    "Lua 解释器一退出，屏幕上的对象全部销毁。\n\n"
                    "**必须满足**：\n"
                    "- 脚本末尾要有常驻主循环：`while true do ... lvgl.process_events(16) end`；\n"
                    "- 所有游戏逻辑（移动、碰撞、计分）都放在这个循环里推进；\n"
                    "- 输出完整可运行脚本，不要只给状态表或片段。"
                )
            elif outcome is not None and not getattr(outcome, "ok", False):
                detail = f"第 {index} 轮在设备上执行失败。\n\n**状态**：{getattr(outcome, 'status', 'unknown')}"
                summary = getattr(outcome, "summary", "")
                log = getattr(outcome, "log", "")
                if summary:
                    detail += f"\n**错误摘要**：{summary}"
                hints = diagnose(summary, log)
                if hints:
                    detail += f"\n\n**问题分析**：\n{hints}"
                if log:
                    detail += f"\n\n**设备日志**：\n```\n{log[:MAX_LOG_ECHO]}\n```"
            else:
                continue

            messages.append({
                "role": "user",
                "content": (
                    f"{detail}\n\n"
                    "**修复要求**：\n"
                    "1. 只改导致这个错误的部分，其余保持原样，不要重写整个脚本。\n"
                    "2. 改之前回到 API 参考核对函数签名，不要凭记忆。\n"
                    "3. 不要提交和上一轮实质相同的代码。\n"
                    "4. 仍然只返回一个 lua 代码块。"
                ),
            })

        if not messages:
            return None
        return ContextSection(
            name="修复历史",
            kind=ContextKind.MESSAGES,
            content=messages,
            priority=30,
        )

    return ContextProvider(name="repair", collect=collect)


def build_default_builder(
    *,
    system_prompt: str = "",
    registry: list[playbook_registry.Playbook] | None = None,
    char_budget: int | None = None,
):
    """按 stable -> dynamic 顺序装配好的 builder。"""
    from context_engine import DEFAULT_CHAR_BUDGET, ContextBuilder

    builder = ContextBuilder(char_budget=char_budget or DEFAULT_CHAR_BUDGET)
    builder.register_all([
        system_rules_provider(system_prompt),
        device_provider(),
        plan_provider(),
        playbook_provider(registry if registry is not None else playbook_registry.load_registry()),
        api_reference_provider(),
        diagnosis_provider(),
        history_provider(),
        repair_provider(),
    ])
    return builder

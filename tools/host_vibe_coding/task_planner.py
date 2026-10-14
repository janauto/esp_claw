#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""LLM 任务规划器：把"写一个贪吃蛇"这类需求拆成可执行的实现计划。

和旧的 plan_modules() 的区别：旧版只让 LLM 吐一个模块名数组，等于把"要用什么"
和"怎么做"合在一次生成里，模型没有中间落脚点。这里让规划阶段先产出结构化的
TaskPlan（目标 / 任务类型 / 界面栈 / 输入方式 / 实现步骤 / 风险 / 验收标准），
再把它作为一段上下文喂给生成阶段。

规划结果同时驱动下游的路径选择：
* modules      -> 决定加载哪些 README（渐进式披露的第二层）
* task_kind    -> 决定挂哪个 playbook（对标设备端按 skill 加载 SKILL.md）
* ui_stack     -> 消解 display / lvgl 互斥，这是设备端 API 最容易混用的地方
* risks        -> 直接进 prompt，让模型提前避开自己预判的坑

只做一次 LLM 调用；JSON 不合规时把校验错误回灌重试一次，仍失败就退回关键词路由。
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from typing import Any, Callable

import lua_module_catalog as catalog

#: 规划阶段的 token 上限。给得比生成阶段小，但必须留够 thinking 模型的推理余量——
#: 旧版给 400 太小，thinking 模型会把额度全烧在 reasoning 上导致 content 为空；
#: 2000 对贪吃蛇这类 8 步计划仍会 finish_reason=length（实测 completion_tokens=2001）。
PLANNER_MAX_TOKENS = 8000

UI_STACKS = ("lvgl", "display", "none")
TASK_KINDS = ("game", "ui", "sensor", "control", "io", "net", "media", "mixed")


@dataclass(frozen=True)
class PlanStep:
    """实现计划里的一步。"""

    order: int
    action: str
    modules: list[str] = field(default_factory=list)

    def describe(self) -> str:
        mods = f"（{', '.join(self.modules)}）" if self.modules else ""
        return f"{self.order}. {self.action}{mods}"


@dataclass
class TaskPlan:
    """规划阶段的完整产出。可变，因为 docs 要在加载 README 后回填。"""

    goal: str = ""
    task_kind: str = ""
    ui_stack: str = "none"
    input_method: str = "none"
    modules: list[str] = field(default_factory=list)
    steps: list[PlanStep] = field(default_factory=list)
    risks: list[str] = field(default_factory=list)
    acceptance: list[str] = field(default_factory=list)
    notes: str = ""
    source: str = "keywords"
    docs: list[str] = field(default_factory=list)
    playbooks: list[str] = field(default_factory=list)

    def describe(self) -> str:
        lines = [f"规划（{self.source}）"]
        if self.goal:
            lines.append(f"  目标：{self.goal}")
        meta = [f"类型={self.task_kind or '未分类'}"]
        if self.ui_stack != "none":
            meta.append(f"界面栈={self.ui_stack}")
        if self.input_method != "none":
            meta.append(f"输入={self.input_method}")
        lines.append("  " + "，".join(meta))
        lines.append(f"  模块：{', '.join(self.modules) or '(无)'}")
        if self.steps:
            lines.append("  步骤：")
            lines.extend(f"    {step.describe()}" for step in self.steps)
        if self.risks:
            lines.append("  风险：" + "；".join(self.risks))
        if self.acceptance:
            lines.append("  验收：" + "；".join(self.acceptance))
        if self.playbooks:
            lines.append("  Playbook：" + ", ".join(self.playbooks))
        if self.docs:
            lines.append("  载入 README：" + ", ".join(self.docs))
        if self.notes:
            lines.append(f"  说明：{self.notes}")
        return "\n".join(lines)

    def as_context(self) -> str:
        """规划结果转成生成阶段的上下文段落。"""
        lines: list[str] = []
        if self.goal:
            lines.append(f"**目标**：{self.goal}")
        if self.task_kind:
            lines.append(f"**任务类型**：{self.task_kind}")
        if self.ui_stack != "none":
            other = "lvgl" if self.ui_stack == "display" else "display"
            lines.append(
                f"**界面栈**：只用 `{self.ui_stack}`，本脚本内不要 require(\"{other}\")。"
            )
        if self.input_method != "none":
            lines.append(f"**输入方式**：{self.input_method}")
        if self.modules:
            lines.append(f"**允许 require 的模块**：{', '.join(self.modules)}")
        if self.steps:
            lines.append("\n**实现步骤**（按此顺序落地，不要跳步）：")
            lines.extend(step.describe() for step in self.steps)
        if self.risks:
            lines.append("\n**已预判的风险，生成时必须规避**：")
            lines.extend(f"- {risk}" for risk in self.risks)
        if self.acceptance:
            lines.append("\n**验收标准**：")
            lines.extend(f"- {item}" for item in self.acceptance)
        return "\n".join(lines)


PLANNER_SYSTEM_PROMPT = """你是 ESP-Claw 设备端 Lua 脚本的实现规划器。用户会描述一个想在设备上跑起来的功能，你负责把它拆成一份可直接落地的实现计划。你不写代码。

只输出一个 JSON 对象，不要 markdown 代码块，不要任何解释文字。字段：
{
  "goal": "一句话说明脚本跑起来后用户会看到/得到什么",
  "task_kind": "game|ui|sensor|control|io|net|media|mixed 之一",
  "ui_stack": "lvgl|display|none 之一",
  "input_method": "lcd_touch|button|knob|imu|adc|none 之一",
  "modules": ["只能取自下方模块清单里出现过的名字"],
  "steps": [{"order":1,"action":"这一步具体做什么","modules":["本步用到的模块"]}],
  "risks": ["这类任务在本设备上最容易踩的坑，每条一句话"],
  "acceptance": ["怎么判断脚本真的成功了，要能从设备日志或屏幕上观察到"]
}

规划规则：
1. modules 只能来自清单，绝不虚构；一般 3-7 个，宁缺勿滥。
2. ui_stack 二选一，绝不同时用 display 和 lvgl。**默认选 lvgl**：
   - 控件界面（按钮/标签/滑块/列表）、触摸事件回调 -> lvgl
   - 游戏和动画也用 lvgl：画面元素做成 lvgl 对象，用 set_pos 增量移动。
     普通点击/按钮/跳跃用 LVGL `on("clicked")` 事件回调；水果忍者、画线、拖拽轨迹、
     滑动切割这类需要连续坐标的任务，规划为 `lcd_touch.poll(handle)` 主循环采样，
     且不要把同一个触摸句柄注册给 LVGL，避免滑动手势滚动画面。
     跳跃闯关、贪吃蛇、俄罗斯方块、打砖块这类都能用 lvgl 对象拼出来，一律选 lvgl。
   - 只有确实需要逐像素自绘、画面无法用对象表达时才选 display。
   - 纯数据采集、纯 IO 控制、不上屏 -> none
3. 用到屏幕必须带 board_manager；有主循环/动画节拍/需要保持画面必须带 delay。
4. steps 要覆盖完整生命周期：初始化 -> 主逻辑 -> 收尾/保持画面。
   游戏类必须显式包含状态初始化、输入采样、状态更新、渲染、结束判定这几步。
5. risks 要具体到 API 层面，不要写"注意性能"这种空话。
   例如"lvgl 颜色是 0xRRGGBB 数字而 display 是字符串"、"set_pos 坐标必须取整"、
   "不要用 get_pos 读回坐标做增量"、"get_display_lcd_params 是多返回值"、
   "全屏游戏要 set_scroll({dir='none', scrollbar='off'})"、
   "隐藏 overlay 不能只改透明度，必须移出屏幕或删除"。
6. 设备是单片机：内存有限、没有协程调度器、脚本跑在一个 Lua job 里。
   不要规划需要多进程、大内存缓冲或外部网络依赖的方案。
"""

PLANNER_SCHEMA_HINT = (
    "上一次返回不符合要求：{error}\n"
    "请重新只输出一个合法 JSON 对象，字段和取值范围严格按系统提示。"
)


def _coerce_str_list(raw: object, *, limit: int = 8) -> list[str]:
    if not isinstance(raw, list):
        return []
    out: list[str] = []
    for item in raw:
        if isinstance(item, str) and item.strip():
            out.append(item.strip())
        if len(out) >= limit:
            break
    return out


def _coerce_enum(raw: object, allowed: tuple[str, ...], fallback: str) -> str:
    value = str(raw or "").strip().lower()
    return value if value in allowed else fallback


def _coerce_steps(raw: object) -> list[PlanStep]:
    if not isinstance(raw, list):
        return []
    steps: list[PlanStep] = []
    for index, item in enumerate(raw[:12], 1):
        if isinstance(item, str):
            if item.strip():
                steps.append(PlanStep(order=index, action=item.strip()))
            continue
        if not isinstance(item, dict):
            continue
        action = str(item.get("action") or item.get("desc") or "").strip()
        if not action:
            continue
        try:
            order = int(item.get("order") or index)
        except (TypeError, ValueError):
            order = index
        steps.append(
            PlanStep(
                order=order,
                action=action,
                modules=catalog.normalize_requires(item.get("modules")),
            )
        )
    return steps


def extract_json_object(text: str) -> dict[str, Any]:
    """从 LLM 回复里取第一个 JSON 对象，容忍 ```json 包裹和前后闲聊。"""
    stripped = (text or "").strip()
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


def _resolve_ui_conflict(modules: list[str], ui_stack: str) -> list[str]:
    """按规划出的 ui_stack 消解 display/lvgl 互斥。

    设备端这两套 API 不能在同一脚本混用，而模型很容易两个都 require。
    规划阶段已经明确选了一个，这里以它为准强制清理。
    """
    if ui_stack == "lvgl":
        return [m for m in modules if m != "display"]
    if ui_stack == "display":
        return [m for m in modules if m != "lvgl"]
    return modules


def parse_plan(payload: dict[str, Any], *, source: str = "llm") -> TaskPlan:
    """把 LLM 的 JSON 转成 TaskPlan，顺带做取值域校验。schema 不合规时抛异常。"""
    modules = catalog.normalize_requires(payload.get("modules"))
    if not modules:
        raise ValueError("modules 为空或全部不在模块清单里")

    ui_stack = _coerce_enum(payload.get("ui_stack"), UI_STACKS, "none")
    #: 模型有时选了 lvgl/display 却漏填 ui_stack，从 modules 反推补齐。
    if ui_stack == "none":
        if "lvgl" in modules:
            ui_stack = "lvgl"
        elif "display" in modules:
            ui_stack = "display"

    modules = catalog.expand_with_implied(_resolve_ui_conflict(modules, ui_stack))

    return TaskPlan(
        goal=str(payload.get("goal") or "").strip(),
        task_kind=_coerce_enum(payload.get("task_kind"), TASK_KINDS, "mixed"),
        ui_stack=ui_stack,
        input_method=str(payload.get("input_method") or "none").strip().lower(),
        modules=modules,
        steps=_coerce_steps(payload.get("steps")),
        risks=_coerce_str_list(payload.get("risks")),
        acceptance=_coerce_str_list(payload.get("acceptance"), limit=6),
        notes=str(payload.get("notes") or "").strip(),
        source=source,
    )


#: 「点击屏幕」这类说法意味着触摸输入，但 lvgl 分支会把 lcd_touch 从 modules 里摘掉。
_TOUCH_HINTS = ("点击", "触摸", "触屏", "点按", "touch", "tap", "click")

#: task_kind 关键词表。顺序即优先级：命中靠前的类别先赢，避免「贪吃蛇游戏」被判成 ui。
_TASK_KIND_HINTS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("game", ("游戏", "game", "贪吃蛇", "snake", "俄罗斯方块", "tetris",
              "打砖块", "breakout", "跳跃", "jump", "得分", "score", "关卡", "障碍")),
    ("sensor", ("传感器", "sensor", "温度", "湿度", "加速度", "陀螺仪", "imu",
                "采集", "读取数据", "adc", "光照")),
    ("control", ("电机", "motor", "舵机", "servo", "pwm", "mcpwm", "控制转速",
                 "占空比", "驱动")),
    ("io", ("gpio", "引脚", "电平", "i2c", "spi", "uart", "串口", "继电器")),
    ("net", ("wifi", "网络", "http", "mqtt", "联网", "请求", "下载", "上传")),
    ("media", ("音频", "播放", "录音", "audio", "喇叭", "麦克风", "摄像头", "拍照")),
    ("ui", ("界面", "按钮", "菜单", "列表", "显示", "屏幕", "ui", "label",
            "文字", "图片", "动画")),
)

#: modules 到 task_kind 的兜底映射，用于提示词没有明显类别词的情况。
_MODULE_KIND_HINTS: tuple[tuple[str, str], ...] = (
    ("imu", "sensor"),
    ("adc", "sensor"),
    ("mcpwm", "control"),
    ("gpio", "io"),
    ("i2c", "io"),
    ("uart", "io"),
    ("lvgl", "ui"),
    ("display", "ui"),
)


def infer_task_kind(prompt: str, modules: list[str]) -> str:
    """按关键词推断 task_kind，失败时回退到 modules 映射。

    task_kind 是 playbook 匹配的主维度，留空会让所有带 task_kind 条件的
    playbook 全部落空，只剩 baseline 生效。
    """
    haystack = prompt.lower()
    for kind, keywords in _TASK_KIND_HINTS:
        if any(kw.lower() in haystack for kw in keywords):
            return kind
    for module, kind in _MODULE_KIND_HINTS:
        if module in modules:
            return kind
    return ""


def fallback_plan(prompt: str, reason: str = "") -> TaskPlan:
    """LLM 规划不可用时的关键词兜底。只保证模块选择可用，没有步骤和风险。"""
    modules = catalog.select_by_keywords(prompt)
    ui_stack = "lvgl" if "lvgl" in modules else ("display" if "display" in modules else "none")
    input_method = "none"
    for candidate in ("lcd_touch", "button", "knob", "imu", "adc"):
        if candidate in modules:
            input_method = candidate
            break
    #: lvgl 内建触摸处理，select_by_keywords 会移除 lcd_touch；这里按提示词补回输入方式，
    #: 否则 input_method 恒为 none，playbook 和提示词都拿不到输入相关约束。
    if input_method == "none" and ui_stack == "lvgl":
        if any(kw in prompt.lower() for kw in _TOUCH_HINTS):
            input_method = "lcd_touch"
    return TaskPlan(
        task_kind=infer_task_kind(prompt, modules),
        modules=modules,
        ui_stack=ui_stack,
        input_method=input_method,
        source="keywords",
        notes=reason,
    )


def plan_task(
    prompt: str,
    call_llm: Callable[[list[dict[str, str]], int], str],
    *,
    device_info: str = "",
) -> TaskPlan:
    """规划入口。

    call_llm 由调用方注入（签名 (messages, max_tokens) -> content），
    这样规划器不依赖具体的 HTTP 客户端，也便于单测里替换成假实现。
    """
    base_messages: list[dict[str, str]] = [
        {"role": "system", "content": PLANNER_SYSTEM_PROMPT},
        {"role": "system", "content": f"可用模块清单：\n{catalog.catalog_overview()}"},
    ]
    if device_info:
        base_messages.append({"role": "system", "content": f"目标设备信息：\n{device_info}"})
    base_messages.append({"role": "user", "content": prompt})

    messages = list(base_messages)
    last_error = ""
    last_raw = ""

    #: 两次机会：第一次直出，不合规就把模型自己的输出和校验错误一起回灌重试一次。
    for attempt in range(2):
        try:
            last_raw = call_llm(messages, PLANNER_MAX_TOKENS)
            plan = parse_plan(extract_json_object(last_raw))
            if last_error:
                plan.notes = (plan.notes + f"（首次规划不合规已重试：{last_error}）").strip()
            return plan
        except Exception as err:
            last_error = f"{type(err).__name__}: {err}"
            if attempt == 0:
                messages = base_messages + [
                    {"role": "assistant", "content": last_raw or "(空回复)"},
                    {"role": "user", "content": PLANNER_SCHEMA_HINT.format(error=last_error)},
                ]

    return fallback_plan(prompt, f"LLM 规划不可用，已退回关键词匹配：{last_error}")

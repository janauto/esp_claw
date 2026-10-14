#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""上位机迭代逻辑的测试，重点是"什么时候不该再花一次 LLM 调用"。

运行方式（任选其一）：

    pytest tools/host_vibe_coding/test_host_vibe.py
    python tools/host_vibe_coding/test_host_vibe.py

不需要真设备：BLE bridge 用假对象替代，LLM 调用被替换成固定返回。
"""

from __future__ import annotations

import asyncio
import dataclasses
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import host_vibe as h  # noqa: E402
import lua_module_catalog as catalog  # noqa: E402
import task_planner  # noqa: E402

GOOD_SCRIPT = 'local display = require("display")\nprint("ok")\n'
LUA_ERROR = "lua:12: attempt to index a nil value (global 'screen')"


def status_text(status, log, runtime=1, summary="(empty)", truncated=False):
    return (
        f"job_id=ab12cd34\nname=req\nstatus={status}\nexclusive=ble_host\n"
        f"runtime_s={runtime}\npath=/fatfs/scripts/host/x.lua\nargs={{}}\n"
        f"summary={summary}\nlog_seq=10\nlog_size=10\n"
        f"log_truncated={'true' if truncated else 'false'}\nrecent_log={log}"
    )


class FakeBridge:
    """按调用顺序返回预设的 job 状态，并记录 job_tail 调用次数。"""

    def __init__(self, statuses, tail="", tail_ok=True):
        self.statuses = list(statuses)
        self.tail = tail
        self.tail_ok = tail_ok
        self.tail_calls = 0
        self.runs = 0
        self.current = ""

    async def request(self, command, timeout=None):
        op = command.get("op")
        if op == "put_lua":
            return {"ok": True, "data": "/fatfs/scripts/host/x.lua"}
        if op == "run_lua":
            self.current = self.statuses[min(self.runs, len(self.statuses) - 1)]
            self.runs += 1
            return {"ok": True, "data": "Started Lua job ab12cd34 (name=req)"}
        if op == "job_status":
            return {"ok": True, "data": self.current}
        if op == "job_tail":
            self.tail_calls += 1
            if not self.tail_ok:
                return {"ok": False, "error": "unknown_op"}
            return {"ok": True, "data": f"job_id=ab12cd34\nstatus=failed\nlog={self.tail}"}
        raise AssertionError(f"未预期的 op: {op}")


def long_run_outcome(text):
    """解析成 JobOutcome 并标成"本该长跑"，等价于 run_lua_and_collect 里的实际接线。"""
    return dataclasses.replace(h.parse_job_status(text), expects_long_run=True)


def run_iterate(scripts, statuses, max_attempts=3, prompt="做一个小游戏"):
    """跑一次迭代循环，返回 (轮次列表, LLM 调用次数)。

    prompt 默认是游戏类，会命中 wants_long_run：这类任务 done+秒退算失败、
    timeout 才算成功。测普通脚本时要显式传一个不含界面词的 prompt。
    """
    calls = {"n": 0}

    def fake_llm(config, messages, max_tokens=None, on_progress=None, on_note=None, cancel=None):
        index = min(calls["n"], len(scripts) - 1)
        calls["n"] += 1
        return f"```lua\n{scripts[index]}\n```"

    original = h.call_llm
    h.call_llm = fake_llm
    try:
        attempts, _ = asyncio.run(h.iterate_lua(
            FakeBridge(statuses),
            h.HostLlmConfig(base_url="http://x", model="m", api_key="k"),
            prompt,
            "device info",
            [],
            plan=h.ModulePlan(modules=["display", "delay"], source="test"),
            max_attempts=max_attempts,
            poll_timeout=0.1,
        ))
    finally:
        h.call_llm = original
    return attempts, calls["n"]


def test_parse_job_status_keeps_multiline_log():
    outcome = h.parse_job_status(status_text("failed", f"{LUA_ERROR}\nstack traceback:", runtime=2))
    assert outcome.job_id == "ab12cd34"
    assert outcome.status == "failed"
    assert outcome.runtime_s == 2
    assert "stack traceback:" in outcome.log
    assert outcome.has_diagnostics


def test_extract_job_id_from_both_text_forms():
    assert h.extract_job_id("Started Lua job ab12cd34 (name=req, timeout_ms=60000)") == "ab12cd34"
    assert h.extract_job_id("Lua job ff00ee11 (name=x) ended early with status=failed.") == "ff00ee11"
    assert h.extract_job_id("no job here") == ""


def test_done_is_success_and_not_retryable():
    outcome = h.parse_job_status(status_text("done", "hello world"))
    assert outcome.ok
    assert not outcome.retryable


def test_long_running_timeout_is_not_retryable():
    """游戏主循环跑满 timeout 是预期行为，不该再花 LLM 调用去"修"。"""
    outcome = h.parse_job_status(status_text("timeout", "frame 1\nframe 2", runtime=60))
    assert not outcome.ok
    assert not outcome.retryable


def test_real_lua_error_is_retryable():
    outcome = h.parse_job_status(status_text("failed", LUA_ERROR))
    assert outcome.retryable
    assert "attempt to index a nil value" in outcome.error_signature()


def test_normalize_script_ignores_comment_only_changes():
    assert h.normalize_script(GOOD_SCRIPT) == h.normalize_script(GOOD_SCRIPT + "-- 换个注释\n")
    assert h.normalize_script(GOOD_SCRIPT) != h.normalize_script(GOOD_SCRIPT + 'print("v2")\n')


def test_success_on_first_attempt_costs_one_call():
    """普通脚本（非界面类）：done 就是成功，一轮一次调用。"""
    attempts, calls = run_iterate(
        [GOOD_SCRIPT], [status_text("done", "hello")], prompt="打印一下内存占用"
    )
    assert calls == 1
    assert len(attempts) == 1
    assert attempts[-1].ok


def test_game_done_in_one_second_is_flicker_not_success():
    """你贴的贪吃蛇日志：status=done runtime_s=1 no output，屏幕闪一下就回主界面。

    以前这里判成功，迭代直接结束；现在必须判失败，并且值得再要一轮。
    """
    outcome = long_run_outcome(
        status_text("done", "(none)", runtime=1, summary="Lua script completed with no output.")
    )
    assert outcome.flickered
    assert not outcome.ok
    assert outcome.retryable
    assert outcome.has_diagnostics, "秒退要能自己合成诊断，否则会因'没有依据'提前停"


def test_game_timeout_is_success_not_failure():
    """跳跃游戏那轮 status=timeout runtime_s=63：主循环还在转，这才是成功。"""
    outcome = long_run_outcome(status_text("timeout", "游戏开始\n跳跃!", runtime=63))
    assert outcome.ok
    assert not outcome.retryable


def test_flicker_retry_tells_model_to_add_main_loop():
    """秒退后下一轮的修复消息里要明确说缺主循环，否则模型改不到点子上。"""
    attempts, calls = run_iterate(
        [GOOD_SCRIPT, GOOD_SCRIPT + 'print("v2")\n'],
        [
            status_text("done", "(none)", runtime=1, summary="Lua script completed with no output."),
            status_text("timeout", "frame", runtime=60),
        ],
    )
    assert calls == 2, "秒退必须触发第二轮生成"
    assert attempts[-1].ok, "第二轮跑成长跑，判成功"
    described = attempts[0].outcome.describe()
    assert "process_events" in described and "主循环" in described


def test_duplicate_script_stops_without_extra_call():
    """LLM 又吐了同一份脚本：立刻停，不再上传执行，也不再请求下一轮。"""
    err = status_text("failed", LUA_ERROR)
    attempts, calls = run_iterate([GOOD_SCRIPT, GOOD_SCRIPT, GOOD_SCRIPT], [err, err, err])
    assert calls == 2
    assert len(attempts) == 2
    assert "实质相同" in attempts[-1].stopped_because


def test_same_error_twice_stops_before_third_call():
    """脚本确实改了但错误一模一样，说明没在收敛，别烧第三次。"""
    err = status_text("failed", LUA_ERROR)
    attempts, calls = run_iterate(
        [GOOD_SCRIPT, GOOD_SCRIPT + 'print("v2")\n', GOOD_SCRIPT + 'print("v3")\n'],
        [err, err, err],
    )
    assert calls == 2
    assert "同一个错误" in attempts[-1].stopped_because


def test_missing_diagnostics_stops_retrying():
    """设备没给 summary 也没给日志，没有依据修脚本。"""
    attempts, calls = run_iterate(
        [GOOD_SCRIPT, GOOD_SCRIPT + 'print("v2")\n'],
        [status_text("failed", "(empty)")],
    )
    assert calls == 1
    assert "缺少可用于修脚本的信息" in attempts[-1].stopped_because


def test_changing_errors_use_full_budget():
    """错误在变化说明每轮都有新信息，这时才值得用满预算。"""
    attempts, calls = run_iterate(
        [GOOD_SCRIPT, GOOD_SCRIPT + 'print("v2")\n', GOOD_SCRIPT + 'print("v3")\n'],
        [
            status_text("failed", LUA_ERROR),
            status_text("failed", "lua:5: bad argument #2 to 'draw_text'"),
            status_text("failed", "lua:9: attempt to call a nil value (field 'present')"),
        ],
    )
    assert calls == 3
    assert len(attempts) == 3


def test_truncated_log_is_refetched_via_job_tail():
    bridge = FakeBridge(
        [status_text("failed", "...被截断的尾巴", truncated=True)],
        tail=f"完整 traceback：{LUA_ERROR}",
    )
    outcome = asyncio.run(h.run_lua_and_collect(bridge, GOOD_SCRIPT, poll_timeout=0.1))
    assert bridge.tail_calls == 1
    assert "完整 traceback" in outcome.log


def test_old_firmware_without_job_tail_falls_back():
    """旧固件没有 job_tail op：回退到 job_status 的尾巴，不能抛异常。"""
    bridge = FakeBridge(
        [status_text("failed", "只有尾巴", truncated=True)],
        tail_ok=False,
    )
    outcome = asyncio.run(h.run_lua_and_collect(bridge, GOOD_SCRIPT, poll_timeout=0.1))
    assert outcome.status == "failed"
    assert "只有尾巴" in outcome.log


def test_catalog_docs_all_exist():
    missing = [item.require for item in catalog.CATALOG if not item.doc_path.exists()]
    assert not missing, f"这些模块的 README 不存在：{missing}"


def test_catalog_selection_pulls_display_dependencies():
    modules = catalog.select_by_keywords("在屏幕上显示 helloworld")
    assert "display" in modules
    assert "board_manager" in modules, "选了 display 就必须带上 board_manager"
    assert "delay" in modules


def test_catalog_selection_prefers_lvgl_for_widgets():
    modules = catalog.select_by_keywords("做一个带按钮和滑块的设置界面")
    assert "lvgl" in modules


def test_normalize_requires_rejects_invented_modules():
    assert catalog.normalize_requires(["display", "screen", "not_a_module"]) == ["display"]
    assert catalog.normalize_requires("display") == []


def test_still_running_reports_accurately_and_does_not_retry():
    """轮询到点脚本还在跑：都不重试，但游戏和一次性脚本的结论相反。"""
    # 游戏：不限时执行下 running 就是正常终局，算成功
    attempts, calls = run_iterate(
        [GOOD_SCRIPT, GOOD_SCRIPT + 'print("v2")\n'],
        [status_text("running", "frame 1\nframe 2", runtime=30)],
    )
    assert calls == 1
    assert attempts[-1].outcome.ok, "游戏还在跑却没判成功"
    assert not attempts[-1].stopped_because

    # 一次性脚本：跑完就该退，卡在 running 不算成功，但也不值得重试
    attempts, calls = run_iterate(
        [GOOD_SCRIPT, GOOD_SCRIPT + 'print("v2")\n'],
        [status_text("running", "frame 1\nframe 2", runtime=30)],
        prompt="读取一次温度并打印",
    )
    assert calls == 1
    assert not attempts[-1].outcome.ok
    assert "还在跑" in attempts[-1].stopped_because


def test_unreadable_job_status_reports_accurately():
    """连 job 状态都读不到时，停止原因不能谎称是长跑脚本。"""

    class BrokenBridge(FakeBridge):
        async def request(self, command, timeout=None):
            if command.get("op") == "job_status":
                return {"ok": False, "error": "job_id_required"}
            return await super().request(command, timeout)

    calls = {"n": 0}

    def fake_llm(config, messages, max_tokens=None, on_progress=None, on_note=None, cancel=None):
        calls["n"] += 1
        return f"```lua\n{GOOD_SCRIPT}\n```"

    original = h.call_llm
    h.call_llm = fake_llm
    try:
        attempts, _ = asyncio.run(h.iterate_lua(
            BrokenBridge([status_text("failed", LUA_ERROR)]),
            h.HostLlmConfig(base_url="http://x", model="m", api_key="k"),
            "任务",
            "",
            [],
            plan=h.ModulePlan(modules=["display"], source="test"),
            max_attempts=3,
            poll_timeout=0.1,
        ))
    finally:
        h.call_llm = original
    assert calls["n"] == 1
    assert "没能读到 job 状态" in attempts[-1].stopped_because


def test_extract_json_object_tolerates_fences_and_chatter():
    payload = h.extract_json_object('好的：\n```json\n{"modules":["display"]}\n```\n')
    assert payload["modules"] == ["display"]


def test_lvgl_doc_is_not_empty():
    """lvgl 的 README 标题是 `## Minimal Example`，之前的正则抽出 0 字符。"""
    docs, paths = catalog.load_module_docs(["lvgl"])
    assert len(docs) > 3000, f"lvgl 文档只有 {len(docs)} 字符，正则又把它截断了"
    assert "lvgl.init" in docs
    assert "Minimal Example" in docs, "`## Minimal Example` 那一节又被正则漏掉了"
    assert paths, "没有回报文档来源路径，GUI 的『载入 README』会是空的"


def test_driver_modules_are_in_catalog():
    """mcpwm/gpio 这类在仓库里是 lua_driver_*，之前整类都查不到。"""
    names = {item.require for item in catalog.CATALOG}
    for name in ("mcpwm", "gpio", "i2c", "uart", "adc"):
        assert name in names, f"{name} 不在 catalog 里"


def test_game_task_routes_to_lvgl_with_touch():
    """游戏走 lvgl 控件 + 事件回调，不走 display 全屏重绘：
    上层接口兼容性更好，且已有可跑通的实机脚本作为依据。
    display 与 lvgl 互斥，选中 lvgl 后不能再出现 display。
    """
    modules = catalog.select_by_keywords("写一个跳跃小游戏，点击屏幕跳起来")
    assert "lvgl" in modules
    assert "display" not in modules, "display 与 lvgl 互斥，不能同时进入模块表"
    assert "lcd_touch" in modules, "点击类游戏必须带上触摸模块"

    plan = task_planner.fallback_plan("写一个跳跃小游戏，点击屏幕跳起来")
    assert plan.ui_stack == "lvgl"
    assert plan.input_method == "lcd_touch"


def test_repair_echoes_previous_script_for_both_failure_kinds():
    """本地校验失败和设备执行失败都要把上一轮脚本原文回灌，否则模型无从定向修改。"""
    import host_context

    class FakeOutcome:
        status = "failed"
        summary = "attempt to call a nil value (field 'draw_rect')"
        log = "E (1) lua: attempt to call a nil value (field 'draw_rect')"
        ok = False

    attempts = [
        h.Attempt(index=1, script="local x = display.foo()", error="未定义的 API: display.foo"),
        h.Attempt(index=2, script="display.begin_frame()", outcome=FakeOutcome()),
    ]
    built = host_context.build_default_builder().build(
        host_context.ContextRequest(
            prompt="写一个跳跃小游戏",
            plan=None,
            attempts=attempts,
            round_index=3,
        )
    )
    blob = "\n".join(m["content"] for m in built.messages)
    assert "display.foo" in blob, "第 1 轮脚本没有回灌"
    assert "display.begin_frame" in blob, "第 2 轮脚本没有回灌"
    assert "未定义的 API" in blob, "本地校验错误没有传给模型"
    assert "draw_rect" in blob, "设备端报错没有传给模型"
    assert any(m["role"] == "assistant" for m in built.messages), "缺少 assistant 轮"


def test_prompt_keeps_output_format_instruction():
    """优化分支曾经把 system_prompt 整个丢掉，模型不再被要求产出代码块。"""
    import host_context

    built = host_context.build_default_builder().build(
        host_context.ContextRequest(prompt="写一个跳跃小游戏", plan=None)
    )
    blob = "\n".join(m["content"] for m in built.messages)
    assert "lua" in blob and "代码块" in blob, "输出格式约束丢了"


def test_diagnose_matches_english_device_errors():
    """设备日志是英文，之前那套中文正则基本不会命中。"""
    import host_context

    assert "draw_rect" in host_context.diagnose("attempt to call a nil value (field 'draw_rect')")
    assert host_context.diagnose("attempt to index a nil value (local 'panel')")
    assert host_context.diagnose("bad argument #1 to display.init (userdata expected, got table)")


def test_http_timeout_scales_with_output_budget():
    """90s 固定超时是 read llm timeout 的根因：大预算必须给更长的读窗口。"""
    small = h.http_timeout_for(2000)
    big = h.http_timeout_for(32000)
    assert big > small, (small, big)
    assert small >= h.HTTP_TIMEOUT_MIN_S
    assert h.http_timeout_for(10_000_000) <= h.HTTP_TIMEOUT_MAX_S
    assert h.http_timeout_for(0) == h.HTTP_TIMEOUT_MIN_S
    assert h.http_timeout_for(-5) == h.HTTP_TIMEOUT_MIN_S


def test_default_budget_fits_a_full_game_script():
    """贪吃蛇被 2000/4096 截断过，默认预算必须一次写得完整份脚本。"""
    assert h.DEFAULT_MAX_TOKENS >= h.LONG_RUN_MIN_TOKENS
    assert task_planner.PLANNER_MAX_TOKENS > 2000


def test_deepseek_preset_is_default_and_has_big_window():
    preset = h.HOST_LLM_PRESETS["deepseek"]
    assert preset.model == "deepseek-v4-pro"
    assert preset.context_window == 1_000_000
    assert preset.max_tokens >= h.LONG_RUN_MIN_TOKENS
    assert h.HOST_LLM_PRESET_ORDER[0] == "deepseek"


def test_progress_reports_phases_not_character_counts():
    """回报的是"在做什么"，不是字数——字数看了也不知道进行到哪一步。"""
    lines = []
    on_progress = h.make_progress_reporter(lines.append)

    on_progress("", False)                  # 连上了，还没有内容
    assert "已连接" in lines[0], lines
    on_progress("", False)                  # 保活帧不该重复刷同一行
    assert len(lines) == 1, lines

    on_progress("我需要先规划", True)        # 思维链是一个独立阶段
    assert "分析需求" in lines[1], lines

    on_progress('local lvgl = require("lvgl")\n', False)
    joined = "\n".join(lines)
    assert "开始输出脚本" in joined, lines
    assert "引入 lvgl 模块" in joined, lines
    assert "字" not in joined, f"不该再回报字数：{lines}"


def test_resident_main_loop_alone_marks_script_long_run():
    """复用已初始化屏幕、只写主循环的脚本也该常驻，不能只认 lvgl.init。"""
    loop_only = 'local lvgl = require("lvgl")\nwhile true do lvgl.process_events(16) end\n'
    assert h.script_expects_long_run(loop_only)
    assert h.resolve_lua_timeout_ms(None, h.script_expects_long_run(loop_only)) == 0

    # 建屏幕的老判据继续有效
    assert h.script_expects_long_run('lvgl.init({})\nlvgl.create_screen()\n')

    # 一次性脚本不受影响，仍然拿到有限窗口
    oneshot = 'local sys = require("system")\nprint(sys.get_heap())\n'
    assert not h.script_expects_long_run(oneshot)
    assert h.resolve_lua_timeout_ms(None, False) == h.DEFAULT_LUA_TIMEOUT_MS


def test_game_gets_unlimited_window_oneshot_gets_a_deadline():
    """游戏必须不限时，一次性脚本必须有上限——否则死循环没人收尸。"""
    assert h.resolve_lua_timeout_ms(None, True) == h.LUA_TIMEOUT_UNTIL_CANCELLED
    assert h.resolve_lua_timeout_ms(None, False) == h.DEFAULT_LUA_TIMEOUT_MS
    # 显式指定一律照办，包括显式的 0
    assert h.resolve_lua_timeout_ms(5000, True) == 5000
    assert h.resolve_lua_timeout_ms(0, False) == 0
    assert h.resolve_lua_timeout_ms(-1, False) == 0


def test_game_still_running_at_poll_deadline_is_success():
    """不限时执行后设备不会再报 timeout，running 才是游戏的主要成功信号。"""
    running = h.JobOutcome(job_id="x", status="running", runtime_s=90,
                           expects_long_run=True)
    assert running.ok, "游戏还在跑却被判失败"
    assert not running.retryable

    # 秒退仍然是失败：画面闪一下就回主界面
    flicker = h.JobOutcome(job_id="x", status="done", runtime_s=1,
                           expects_long_run=True)
    assert not flicker.ok and flicker.flickered

    # 刚起步就还在 running，不足以证明画面撑住了
    early = h.JobOutcome(job_id="x", status="running", runtime_s=1,
                         expects_long_run=True)
    assert not early.ok

    # 一次性脚本卡在 running 依然不算成功
    oneshot = h.JobOutcome(job_id="x", status="running", runtime_s=90,
                           expects_long_run=False)
    assert not oneshot.ok


def test_poll_reports_still_running_while_status_unchanged():
    """长跑脚本一直停在 running，状态不变也得间歇报一声，不能整段静默。"""
    class StuckBridge:
        async def request(self, command, timeout=None):
            return {"ok": True, "data": status_text("running", "", runtime=3)}

    events = []
    original = h.JOB_HEARTBEAT_S
    h.JOB_HEARTBEAT_S = 0.0        # 每轮都该报，免得测试真的等 15s
    try:
        outcome = asyncio.run(h.poll_job_status(
            StuckBridge(), "ab12cd34",
            poll_timeout=0.05, poll_interval=0.01,
            on_event=events.append,
        ))
    finally:
        h.JOB_HEARTBEAT_S = original

    assert outcome.status == "running", outcome
    assert events[0] == "job 状态：running", events
    beats = [line for line in events if "仍在设备上运行" in line]
    assert beats, f"状态不变时没有任何心跳：{events}"
    assert "job 状态：running" not in beats[0]      # 心跳不是重复刷状态行


def test_progress_phases_never_go_backwards():
    """SSE 每帧之间有空行，思维链也可能和正文交错——阶段绝不能横跳。"""
    lines = []
    on_progress = h.make_progress_reporter(lines.append)

    on_progress("", False)
    on_progress("while true do\n", False)      # 进入正文
    before = len(lines)

    on_progress("", False)                     # 帧间空行：不该退回"已连接"
    on_progress("再想想边界条件", True)          # 迟到的思维链：不该退回"分析需求"
    on_progress("", False)
    assert len(lines) == before, f"阶段回退了：{lines[before:]}"


def test_progress_recognizes_landmarks_across_chunk_boundaries():
    """路标会被 SSE 切断，跨 chunk 也必须认出来，且不重复报。"""
    lines = []
    on_progress = h.make_progress_reporter(lines.append)

    on_progress("local function draw_sn", False)   # 函数名被切成两半
    on_progress("ake(x, y)\n", False)
    assert any("draw_snake()" in line for line in lines), lines

    before = len(lines)
    on_progress("local function draw_snake(a)\n", False)  # 同一个路标不该报第二次
    assert len(lines) == before, lines

    on_progress("while true do\n", False)
    assert any("主循环" in line for line in lines), lines


def test_stream_unsupported_is_distinct_from_other_failures():
    """只有"服务不认 stream"才可退回非流式；截断之类必须直接抛。

    之前用报错字符串匹配来判断，会把"输出被截断"误判成可重试，
    白烧一次请求还把真正的原因盖掉。
    """
    assert issubclass(h.StreamUnsupported, RuntimeError)
    # 鉴权和限流不在可重试集合里：重试治不好，只是白费一个请求
    assert 401 not in h.STREAM_UNSUPPORTED_HTTP_CODES
    assert 403 not in h.STREAM_UNSUPPORTED_HTTP_CODES
    assert 429 not in h.STREAM_UNSUPPORTED_HTTP_CODES
    assert 400 in h.STREAM_UNSUPPORTED_HTTP_CODES


def test_stall_timeout_is_much_smaller_than_total_budget():
    """流式下超时的语义是"静默间隔"，不该跟着 max_tokens 一起放大。

    非流式必须按总耗时给超时，长脚本能跑到 1800s——真卡死了也得干等。
    流式有了增量输出，卡住能在 90s 内认出来。
    """
    assert h.HTTP_STREAM_STALL_TIMEOUT_S < h.http_timeout_for(32000)
    assert h.HTTP_STREAM_STALL_TIMEOUT_S <= h.HTTP_TIMEOUT_MIN_S


def test_iterate_passes_progress_callback_to_llm():
    """迭代循环必须把进度回调传下去，否则生成期间界面依然是死的。"""
    seen = {}

    def fake_llm(config, messages, max_tokens=None, on_progress=None, on_note=None, cancel=None):
        seen["on_progress"] = on_progress
        return f"```lua\n{GOOD_SCRIPT}\n```"

    original = h.call_llm
    h.call_llm = fake_llm
    try:
        asyncio.run(h.iterate_lua(
            FakeBridge([{"ok": True, "data": status_text("timeout", "ok", runtime=30)}]),
            h.HostLlmConfig(base_url="http://x", model="m", api_key="k"),
            "做个贪吃蛇",
            "device info",
            [],
            plan=h.ModulePlan(modules=["display", "delay"], source="test"),
            max_attempts=1,
            poll_timeout=0.1,
        ))
    finally:
        h.call_llm = original

    assert callable(seen.get("on_progress")), "生成时没有传进度回调"


def main():
    """不装 pytest 也能跑。"""
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and callable(fn)]
    failed = []
    for name, fn in tests:
        try:
            fn()
            print(f"[PASS] {name}")
        except Exception as err:
            failed.append(name)
            print(f"[FAIL] {name}: {err}")
    print(f"\n{len(tests) - len(failed)}/{len(tests)} 通过")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

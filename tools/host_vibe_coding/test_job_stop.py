#!/usr/bin/env python3
"""
测试 job 停止功能的脚本。

用法：
    python test_job_stop.py --device <BLE地址>
"""

import argparse
import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from host_vibe import (
    BleBridge,
    DEFAULT_DEVICE_PREFIX,
    DEFAULT_TIMEOUT,
    request_id,
    run_lua_and_collect,
    stop_job_and_wait,
)

# 一个会长时间运行的测试脚本
TEST_SCRIPT_LONG_RUN = """
local board_manager = require("board_manager")
local delay = require("delay")
local display = require("display")

-- 初始化
local lcd_params = board_manager.get_display_lcd_params()
display.init(lcd_params)
display.clear()

-- 计数器
local count = 0

-- 主循环：每秒更新一次显示
while true do
    count = count + 1
    display.clear()
    display.draw_text(10, 10, "Running: " .. count .. "s", 2, "#FFFFFF")
    delay.ms(1000)
end
"""


async def test_stop_job(device_addr: str | None, name_prefix: str) -> None:
    """测试 job 停止功能。"""
    print("=== Job 停止功能测试 ===\n")

    events = []

    def on_event(message: str) -> None:
        print(f"[事件] {message}")
        events.append((time.time(), message))

    def on_device_event(event: dict) -> None:
        print(f"[设备推送] {event}")
        events.append((time.time(), f"device_event: {event}"))

    bridge = BleBridge(device_addr, name_prefix, DEFAULT_TIMEOUT, on_device_event=on_device_event)

    try:
        print(f"1. 连接设备 (前缀: {name_prefix})...")
        await bridge.connect()
        print("✓ 已连接\n")

        print("2. 启动长时间运行的测试脚本...")
        start_time = time.time()

        # 启动异步任务执行脚本
        run_task = asyncio.create_task(
            run_lua_and_collect(
                bridge,
                TEST_SCRIPT_LONG_RUN,
                timeout=DEFAULT_TIMEOUT,
                lua_timeout_ms=0,  # 不限时运行
                poll_timeout=5,  # 只等 5 秒就返回（脚本还在跑）
                on_event=on_event,
            )
        )

        # 等待 3 秒让脚本启动
        await asyncio.sleep(3)
        print(f"✓ 脚本已运行约 3 秒\n")

        print("3. 发送停止请求（使用新的快速停止函数）...")
        stop_start = time.time()
        stop_resp, final_status = await stop_job_and_wait(
            bridge,
            job_id=None,
            timeout=5,
            wait_after_stop=2.0,
        )
        stop_elapsed = time.time() - stop_start
        print(f"✓ 停止请求完成，总耗时 {stop_elapsed:.2f}s")
        print(f"   停止响应: {stop_resp.get('data') or stop_resp.get('error')}")
        print(f"   最终状态: {final_status.status}\n")

        # 等待异步任务完成或超时
        try:
            outcome = await asyncio.wait_for(run_task, timeout=3)
            print(f"4. 执行结果: {outcome.status}")
        except asyncio.TimeoutError:
            print("4. run_lua_and_collect 仍在等待（预期行为）")
            run_task.cancel()

        total_elapsed = time.time() - start_time
        print(f"\n=== 测试完成 ===")
        print(f"总耗时: {total_elapsed:.2f}s")
        print(f"停止耗时: {stop_elapsed:.2f}s")

        if stop_elapsed < 3:
            print("✓ 停止响应时间正常（< 3s）")
        else:
            print(f"⚠ 停止响应时间较长（{stop_elapsed:.2f}s）")

        print(f"\n事件时间线：")
        first_time = events[0][0] if events else start_time
        for t, msg in events:
            print(f"  +{t - first_time:.2f}s  {msg}")

    finally:
        print("\n断开连接...")
        await bridge.disconnect()


def main() -> None:
    parser = argparse.ArgumentParser(description="测试 job 停止功能")
    parser.add_argument("--device", help="BLE 设备地址")
    parser.add_argument("--name-prefix", default=DEFAULT_DEVICE_PREFIX, help="设备名前缀")
    args = parser.parse_args()

    try:
        asyncio.run(test_stop_job(args.device, args.name_prefix))
    except KeyboardInterrupt:
        print("\n用户中断")
    except Exception as err:
        print(f"错误: {err}", file=sys.stderr)
        raise SystemExit(1) from err


if __name__ == "__main__":
    main()

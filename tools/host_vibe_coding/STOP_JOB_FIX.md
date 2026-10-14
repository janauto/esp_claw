# Job 停止延迟修复说明

## 问题描述

上位机在传递代码给设备执行后，调用 `job_stop` 无法立即停止，必须等很长时间才可以。

## 根本原因

1. **设备端等待时间过长**：`ble_host_bridge.c` 中调用 `cap_lua_stop_job(job_id, 3000, ...)` 时等待 3 秒
2. **上位机超时设置过长**：GUI 中默认请求超时 20 秒
3. **缺少实时反馈**：上位机没有监听设备主动推送的 job 事件，只能通过轮询获取状态

## 修复方案

### 1. 设备端优化 (`ble_host_bridge.c:606`)

```c
// 原代码：等待 3000ms
err = cap_lua_stop_job(job_id, 3000, output, BLE_HOST_BRIDGE_OUTPUT_MAX);

// 修改后：缩短到 1500ms
err = cap_lua_stop_job(job_id, 1500, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
```

**理由**：大多数 Lua 脚本可以在 1.5 秒内响应停止请求。如果脚本有死循环或长时间阻塞，即使等 3 秒也无法优雅停止，最终仍会超时强制终止。

### 2. 上位机 GUI 优化 (`host_vibe_gui.py`)

#### 2.1 缩短停止请求的超时时间

```python
async def job() -> Any:
    # 发送停止请求（缩短超时时间到 5 秒）
    stop_resp = await self.bridge.request({"id": request_id("stop"), "op": "job_stop"}, timeout=5)
    # 等待一小段时间让设备处理停止
    await asyncio.sleep(0.3)
    # 再查询一次状态确认
    status_resp = await self.bridge.request({"id": request_id("job"), "op": "job_status"}, timeout=3)
    return {"stop": stop_resp, "status": status_resp}
```

#### 2.2 添加设备事件监听

连接时注册事件回调：

```python
def on_event(event: dict) -> None:
    # 把设备事件放入结果队列，让主线程处理
    self.result_queue.put(("device_event", event, None))

bridge = BleBridge(target, prefix, 15, on_device_event=on_event)
```

在 `_poll_results` 中处理设备事件：

```python
elif name == "device_event":
    # 处理设备主动推送的事件（如 job 结束）
    event = result or {}
    event_type = event.get("type", "")
    data = event.get("data", {})
    if event_type == "job_event":
        job_id = data.get("job_id", "")
        status = data.get("status", "")
        self.log_text.insert("end", f"[设备事件] job {job_id} 状态: {status}\n")
        self.log_text.see("end")
        if status in {"stopped", "done", "failed", "timeout"}:
            self.status_var.set(f"Job 已{status}")
```

### 3. 预期效果

- **停止响应时间**：从之前的 20+ 秒缩短到 2-3 秒
- **实时反馈**：设备 job 状态变化时立即在日志区显示
- **用户体验**：点击"停止 job"后能快速看到结果，不再需要长时间等待

## 其他建议

### Lua 脚本编写规范

为了确保脚本能快速响应停止请求，在长时间运行的循环中应该：

```lua
-- 不推荐：死循环无检查点
while true do
    -- 大量计算...
end

-- 推荐：定期调用 lvgl.process_events 或 delay
while true do
    -- 业务逻辑...
    lvgl.process_events(16)  -- 给 cap_lua 一个检查停止标志的机会
end
```

### CLI 版本

CLI 工具 (`host_vibe.py`) 中的 `job_stop` 已经使用了合理的超时（默认 15 秒），但也可以考虑缩短到 5 秒。

## 测试建议

1. **快速停止测试**：运行一个带 `while true` 循环的游戏脚本，点击停止，观察响应时间
2. **死循环测试**：运行一个纯计算的死循环（不调用 `lvgl.process_events`），确认超时机制正常工作
3. **多次停止测试**：连续启动和停止多个 job，确认没有资源泄漏或状态混乱

## 文件修改清单

1. `/home/pc/project/esp_claw/application/edge_agent/components/ble_host_bridge/src/ble_host_bridge.c`
2. `/home/pc/project/esp_claw/tools/host_vibe_coding/host_vibe_gui.py`

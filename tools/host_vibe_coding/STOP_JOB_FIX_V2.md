# Job 停止和执行卡顿问题修复 v2

## 问题症状

1. ❌ **点击"停止 job"后无响应**，必须等很长时间
2. ❌ **执行已写好的 Lua 脚本会卡住**，界面无响应
3. ❌ **GUI 报错**：`AttributeError: 'str' object has no attribute 'get'`

## 根本原因分析

### 1. 轮询参数设置不合理

```python
JOB_POLL_INTERVAL = 2.5  # 轮询间隔太长，感觉"卡顿"
JOB_POLL_TIMEOUT = 90.0  # 超时太长，停不下来
```

**影响**：
- 每 2.5 秒才查询一次状态，用户感觉界面卡住
- 默认等待 90 秒才超时，停止 job 时仍在长时间轮询
- GUI 默认使用 90 秒超时，导致执行脚本时界面"卡住"

### 2. 停止逻辑问题

原来的流程：
```
发送 job_stop → 继续调用 poll_job_status(90秒) → 等待终态或超时
```

**问题**：停止请求发送后，仍在做 90 秒的长轮询，而不是快速确认后返回。

### 3. 设备事件解析错误

```python
data = event.get("data", {})
job_id = data.get("job_id", "")  # data 可能是字符串，不是字典
```

## 完整修复方案

### 修改 1: 优化轮询参数 (`host_vibe.py:74-77`)

```python
JOB_POLL_INTERVAL = 1.0   # 从 2.5s 缩短到 1s，响应更快
JOB_POLL_TIMEOUT = 30.0   # 从 90s 缩短到 30s
```

**理由**：
- 1 秒的轮询间隔在 BLE 通信中是合理的
- 30 秒足够大多数脚本执行或达到稳定状态
- 对于真正的长时间运行脚本（游戏等），用户可以手动调整超时

### 修改 2: 添加快速停止函数 (`host_vibe.py`)

```python
async def stop_job_and_wait(
    bridge: BleBridge,
    job_id: str | None = None,
    *,
    timeout: float = DEFAULT_TIMEOUT,
    wait_after_stop: float = 2.0,
) -> tuple[dict[str, Any], JobOutcome]:
    """发送停止请求并短暂等待确认，不做长时间轮询。"""
    # 1. 发送停止请求
    stop_resp = await bridge.request(...)
    
    # 2. 等待 0.5 秒让设备处理
    await asyncio.sleep(0.5)
    
    # 3. 轮询几次确认（最多 2 秒）
    while time.monotonic() - started < wait_after_stop:
        status_resp = await bridge.request(...)
        if status in TERMINAL_JOB_STATUS:
            break  # 已到终态，立即返回
        await asyncio.sleep(0.3)
    
    return stop_resp, last_outcome
```

**优势**：
- 不做长轮询，最多等待 2-3 秒就返回
- 如果 job 快速响应停止，立即返回
- 即使 job 没有完全停止，也会快速给出反馈

### 修改 3: GUI 使用新的停止函数 (`host_vibe_gui.py`)

```python
def _job_stop(self) -> None:
    async def job() -> Any:
        # 使用快速停止，只等 2 秒
        stop_resp, final_status = await stop_job_and_wait(
            self.bridge,
            job_id=None,
            timeout=5,
            wait_after_stop=2.0,
        )
        return {"stop": stop_resp, "status": final_status}
```

### 修改 4: 修复设备事件解析 (`host_vibe_gui.py`)

```python
elif name == "device_event":
    event = result or {}
    event_type = event.get("type", "")
    data = event.get("data", {})
    
    # 处理 data 可能是字符串的情况
    if isinstance(data, str):
        self.log_text.insert("end", f"[设备事件] {event_type}: {data}\n")
    elif event_type == "job_event" and isinstance(data, dict):
        job_id = data.get("job_id", "")
        status = data.get("status", "")
        # ...
```

### 修改 5: 缩短 GUI 默认超时 (`host_vibe_gui.py`)

```python
self.poll_timeout = tk.StringVar(value="15")  # 从 90 缩短到 15 秒
```

**理由**：15 秒对大多数脚本已经足够，避免"卡住"的感觉。

### 修改 6: 设备端缩短等待时间 (`ble_host_bridge.c:606`)

```c
err = cap_lua_stop_job(job_id, 1500, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
// 从 3000ms 缩短到 1500ms
```

## 预期效果对比

### 停止 Job

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 正常响应停止的脚本 | 20+ 秒 | **2-3 秒** |
| 死循环脚本（无响应） | 90+ 秒 | **2-3 秒**（快速超时） |
| 用户体验 | 感觉卡住，无反馈 | 快速返回，有明确状态 |

### 执行脚本

| 场景 | 修复前 | 修复后 |
|------|--------|--------|
| 快速脚本（< 5秒） | 感觉卡 2.5-5 秒 | **1-2 秒返回** |
| 长时间运行脚本 | 界面卡 90 秒 | **15 秒超时或手动调整** |
| 轮询频率 | 每 2.5 秒 | **每 1 秒**（更流畅） |

## 使用建议

### 1. 执行短脚本（测试、查询状态等）

保持默认设置：
- 等待时间：15 秒
- 通常 1-5 秒就能完成

### 2. 执行长脚本（游戏、动画等）

手动调大等待时间到 60-120 秒，或者：
- 不等待完成，直接点"停止 job"中断轮询
- 使用"查询 job 状态"手动检查

### 3. 停止 Job

现在只需 2-3 秒就能得到反馈：
- 脚本正常停止 → 状态显示 "stopped"
- 脚本无响应 → 2 秒后显示当前状态（可能仍是 "running"）
- 设备会在后台继续尝试停止（最多 1.5 秒）

## Lua 脚本编写建议

为了让脚本能快速响应停止请求：

```lua
-- ❌ 不推荐：纯计算循环，无法停止
while true do
    -- 大量计算...
end

-- ✅ 推荐：定期调用 LVGL 事件处理
while true do
    -- 业务逻辑...
    lvgl.process_events(16)  -- 给系统检查停止信号的机会
end

-- ✅ 推荐：使用 delay
while true do
    -- 业务逻辑...
    delay.ms(100)  -- 也会让出 CPU 并检查停止
end
```

## 测试

```bash
cd /home/pc/project/esp_claw/tools/host_vibe_coding

# 测试停止功能
python test_job_stop.py

# 测试 GUI
python host_vibe_gui.py
```

## 文件修改清单

1. `tools/host_vibe_coding/host_vibe.py`
   - 轮询参数优化 (L74-77)
   - 新增 `stop_job_and_wait()` 函数

2. `tools/host_vibe_coding/host_vibe_gui.py`
   - 导入新函数
   - `_job_stop()` 使用快速停止
   - 修复设备事件解析
   - 缩短默认超时到 15 秒

3. `application/edge_agent/components/ble_host_bridge/src/ble_host_bridge.c`
   - 设备端等待时间缩短到 1500ms

## 故障排查

### 问题：停止后状态仍显示 "running"

**原因**：脚本是纯计算循环，没有调用 `lvgl.process_events()` 或 `delay.ms()`

**解决**：修改脚本添加事件处理调用，或等待设备端 1.5 秒超时强制终止

### 问题：执行脚本仍感觉"卡"

**检查**：
1. 查看日志区是否有"job 状态：running"等输出（每 1 秒更新）
2. 如果完全无输出，可能是 BLE 连接问题
3. 尝试断开重连设备

### 问题：GUI 报其他错误

**调试**：
1. 查看完整错误堆栈
2. 确认使用的是修复后的代码
3. 尝试重启 GUI

## 后续改进方向

1. **添加"取消"按钮**：允许用户主动中断正在进行的轮询
2. **进度条**：显示已等待时间 / 总超时时间
3. **自适应超时**：根据脚本类型自动选择合适的超时值
4. **离线模式**：不等待 job 完成，后台轮询，完成时通知用户

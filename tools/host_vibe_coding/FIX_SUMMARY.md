# 上位机 Job 停止和执行卡顿问题修复总结

## 问题

1. 点击"停止 job"后必须等很长时间（20-90秒）
2. 执行 Lua 脚本时界面卡住，无响应
3. GUI 报错：`AttributeError: 'str' object has no attribute 'get'`

## 根本原因

- **轮询参数设置不当**：间隔 2.5s 太长，超时 90s 过久
- **停止逻辑缺陷**：发送停止请求后仍在做长轮询
- **GUI 默认超时过长**：执行脚本时等待 90 秒
- **设备事件解析错误**：未处理 `data` 为字符串的情况

## 核心修复

### 1. 优化轮询参数 (`host_vibe.py`)

```python
JOB_POLL_INTERVAL = 1.0   # 从 2.5s → 1s
JOB_POLL_TIMEOUT = 30.0   # 从 90s → 30s
```

### 2. 新增快速停止函数 (`host_vibe.py`)

```python
async def stop_job_and_wait(bridge, job_id=None, *, 
                             timeout=15.0, wait_after_stop=2.0):
    """发送停止请求，只等待 2 秒确认，不做长轮询"""
```

### 3. GUI 使用快速停止 (`host_vibe_gui.py`)

```python
stop_resp, final_status = await stop_job_and_wait(
    self.bridge, timeout=5, wait_after_stop=2.0
)
```

### 4. 缩短 GUI 默认超时

```python
self.poll_timeout = tk.StringVar(value="15")  # 从 90s → 15s
```

### 5. 修复设备事件解析

```python
if isinstance(data, str):
    # 处理字符串情况
elif isinstance(data, dict):
    # 处理字典情况
```

### 6. 设备端优化 (`ble_host_bridge.c`)

```c
cap_lua_stop_job(job_id, 1500, ...)  // 从 3000ms → 1500ms
```

## 效果

| 操作 | 修复前 | 修复后 |
|------|--------|--------|
| 停止 job | 20-90秒 | **2-3秒** |
| 执行短脚本 | 卡 2.5-5秒 | **1-2秒** |
| 轮询频率 | 每 2.5秒 | **每 1秒** |
| 默认超时 | 90秒 | **15秒** |

## 测试

```bash
# 启动 GUI
python host_vibe_gui.py

# 运行测试脚本
python test_job_stop.py
```

## 注意事项

1. **长时间运行的脚本**（游戏、动画）：手动将"等待执行结果秒数"调到 60-120 秒
2. **脚本应定期让出 CPU**：在循环中调用 `lvgl.process_events(16)` 或 `delay.ms(100)`
3. **停止无响应的脚本**：设备会在 1.5 秒后超时强制终止

## 详细说明

- 完整技术文档：`STOP_JOB_FIX_V2.md`
- 原始分析：`STOP_JOB_FIX.md`

## 修改的文件

1. `tools/host_vibe_coding/host_vibe.py` - 轮询优化 + 快速停止函数
2. `tools/host_vibe_coding/host_vibe_gui.py` - GUI 集成快速停止 + 修复错误
3. `application/edge_agent/components/ble_host_bridge/src/ble_host_bridge.c` - 设备端超时缩短
4. `tools/host_vibe_coding/test_job_stop.py` - 测试脚本更新

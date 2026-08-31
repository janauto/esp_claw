# ESP-Claw 电脑端 Vibe Coding

这个目录是运行在电脑上的独立上位机程序，不属于设备端 `http_server`。

数据流：

```text
电脑 CLI
  ├─ 本地 LLM 配置与历史（~/.esp_claw/host_vibe_coding）
  ├─ OpenAI-compatible API 生成 Lua
  └─ BLE GATT
       └─ ESP-Claw C ble_host_bridge
            └─ 设备端 cap_lua 执行 Lua
```

设备端的 LLM 配置、NVS 和 HTTP 配置页不会被本程序读取或修改。

## 安装

```bash
cd tools/host_vibe_coding
python -m venv .venv
.venv\\Scripts\\activate
python -m pip install -r requirements.txt
```

Linux/macOS 激活命令：

```bash
source .venv/bin/activate
```

## 使用

扫描 BLE bridge：

```bash
python host_vibe.py scan
```

配置电脑端 LLM：

```bash
python host_vibe.py config \
  --base-url https://api.openai.com/v1 \
  --model gpt-4o-mini \
  --api-key YOUR_KEY
```

获取设备上下文：

```bash
python host_vibe.py info
```

使用自然语言生成 Lua，并确认后上传执行：

```bash
python host_vibe.py ask "读取设备内存和网络状态并打印结果"
```

自动确认上传执行：

```bash
python host_vibe.py ask --yes "读取设备内存和网络状态并打印结果"
```

也可以直接上传本地 Lua：

```bash
python host_vibe.py run-lua ./example.lua
```

如果设备扫描不到，可显式指定 BLE 地址：

```bash
python host_vibe.py --device AA:BB:CC:DD:EE:FF info
```

API Key 也可以只通过环境变量提供，不写入配置文件：

```bash
set ESP_CLAW_HOST_LLM_API_KEY=YOUR_KEY
python host_vibe.py ask "生成 Lua"
```

## 图形界面

启动 Tkinter 图形界面：

```bash
python host_vibe_gui.py
```

界面支持点击完成以下操作：

- 配置并保存电脑端 `Base URL`、模型、API Key 和最大 Token；
- 扫描、选择、连接和断开 ESP-Claw BLE bridge；
- 获取设备信息，作为上位机 LLM 上下文；
- 输入自然语言需求并生成 Lua；
- 编辑、打开、保存 Lua，并上传到设备执行；
- 查询或停止设备端 Lua job，查看执行日志。

GUI 使用 Python 自带的 Tkinter，不新增第三方 GUI 依赖。BLE 和 LLM 网络操作运行在后台线程，界面不会被请求阻塞。电脑端配置和历史默认保存在 `~/.esp_claw/host_vibe_coding/`，不会写入设备端 NVS。
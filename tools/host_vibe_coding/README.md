# ESP-Claw 电脑端图形上位机

这个目录是运行在电脑上的独立上位机程序，不属于设备端 `http_server`。

数据流：

```text
电脑 CLI
  ├─ 本地 LLM 配置与历史（~/.esp_claw/host_vibe_coding）
  ├─ 第一阶段：LLM 判断需要哪些设备模块（lua_module_catalog.py）
  ├─ 第二阶段：按需载入对应 README 后生成 Lua
  └─ BLE GATT
       └─ ESP-Claw C ble_host_bridge
            ├─ 设备端 cap_lua 执行 Lua
            └─ job_status / job_tail 回传真实执行结果
```

设备端的 LLM 配置、NVS 和 HTTP 配置页不会被本程序读取或修改。

## 两阶段上下文

`components/lua_modules/` 下有 40 个可 require 的模块，README 合计约 135 KB。
一次性全部塞进请求既浪费 token，也会把关键 API 约束淹掉，所以分两步走：

1. **路由**：把 `lua_module_catalog.py` 里每个模块的一行描述（约 1.6 KB）交给 LLM，
   让它判断这次要用哪些能力，失败时退回关键词匹配；
2. **生成**：只载入选中模块的 README 作为上下文，再生成 Lua。

选到 `display` 会自动补上 `board_manager` 和 `delay`，选到 `lvgl`/`lcd_touch` 会自动补上
`board_manager`——这些是初始化的前置依赖，LLM 经常漏。

想在不连设备的情况下检查模块选择是否合理：

```bash
python host_vibe.py plan "做一个贪吃蛇小游戏，用触摸控制"
python host_vibe.py modules          # 打印完整模块清单
```

新增模块后，在 `lua_module_catalog.py` 的 `CATALOG` 里补一条即可，
`test_host_vibe.py` 会校验每个条目的 README 路径确实存在。

## 迭代执行

设备端 `run_lua` 是异步的，只返回 job id；真正的执行结果要靠 `job_status` 取回。
上位机会自动轮询到终态，`job_status` 的日志尾巴被截断时再用 `job_tail` 拉完整日志。

```bash
python host_vibe.py ask --iterate "做一个贪吃蛇小游戏，用触摸控制"
python host_vibe.py ask --iterate 5 "…"   # 指定最多轮数
```

`--iterate` 会把设备返回的真实报错回灌给 LLM 再生成一版。为了不白烧 token，
遇到下面几种情况会**立即停止**而不是用满轮数：

| 情况 | 为什么不重试 |
|------|--------------|
| `status=done` | 已经成功 |
| `status=timeout` 且有日志输出 | 脚本是长跑的（游戏主循环、交互界面），跑满超时是预期行为 |
| `status=stopped` | 是有人主动停的 |
| 新脚本与上一版实质相同（忽略注释/空白） | LLM 没有真正响应报错，再试结果一样 |
| 连续两轮同一个错误签名 | LLM 没有收敛到解 |
| 设备没返回 summary 也没返回日志 | 没有可用于修脚本的信息，改动等于瞎猜 |

只有真正的 Lua 报错（`status=failed`）且拿到了诊断信息，才会消耗下一轮。
每次运行结束都会打印每轮的状态和停止原因，方便判断 token 花在哪。

如果已经构建了 `tools/lua_lvgl_web_sim`，可以在上传真实设备前先跑 WebAssembly 模拟器预检：

```bash
python -m pip install playwright
python -m playwright install chromium
python host_vibe.py ask --iterate --web-preview-check "做一个水果忍者游戏"
```

GUI 里勾选“上传前自动 Web 预览”即可启用同样流程。它会用真实 Web LVGL/display 模拟器
headless 运行每轮 Lua，捕获 `Lua error`、参数类型错误、模拟器退出码、常驻脚本秒退和
疑似空白屏。每次检查会保存一张 canvas 截图到系统临时目录，并输出基础视觉指标
（非黑像素比例、亮色比例、颜色数量），用于判断“脚本没报错但画面没画出来”的情况。
Web 预览失败时不会上传设备，而是把模拟器日志作为一轮失败反馈给 LLM 修复。
如果没有安装 Playwright 或没找到模拟器构建产物，会自动跳过，不影响原来的设备迭代。

这一步是自动预筛选，不等于完整审美评审。布局拥挤、按钮太小、视觉风格不符合预期这类问题，
仍建议点击 GUI 里的“真实 LVGL Web 预览”直接用浏览器查看，再把观察结果写到“代码反馈”里重生成。
后续如果接入支持图片输入的 LLM，可以把这里保存的截图也作为视觉反馈自动回灌。

不带 `--iterate` 时行为不变：生成一版、确认、执行一次。

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
# 走预设（会连带填好 base_url / model / 接口风格）
python host_vibe.py config --provider anthropic --api-key YOUR_KEY

# 或全部手填。--api-style 决定用哪种接口：
#   openai_chat -> /chat/completions（DeepSeek / Kimi / 百炼 / vLLM / Ollama …）
#   anthropic   -> /v1/messages（Anthropic 原生，x-api-key + anthropic-version 头）
python host_vibe.py config \
  --base-url https://api.openai.com/v1 \
  --model gpt-4o-mini \
  --api-key YOUR_KEY \
  --api-style openai_chat

# Base URL 三种合法填法，选一种即可：
#   https://api.deepseek.com/v1              -> 自动追加 /chat/completions
#   https://api.deepseek.com/v1/chat/completions  -> 原样使用（你粘了完整地址）
#   https://gw.example.com/weird/path#      -> 末尾 # = 锁定这个地址，别追加路径
# 保存后 config 命令会打印 _resolved_endpoint，那就是实际请求的地址，
# 万一 404 一眼就能看出是不是路径拼重了。
```

排查"请求失败但看不出为什么"：

```bash
python host_vibe.py check
```

一次点击就能分清地址错 / Key 错 / 模型名错 / 网络不通，会先读一次 `/models`
再发一次最小生成，不用连设备，也不必拿一次完整生成去试。

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

也可以直接上传本地 Lua（会等待并打印设备端的真实执行结果）：

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

- 配置并保存电脑端 `Base URL`、模型、API Key、**接口风格**和最大 Token；
  - 保存前界面会先显示"实际请求地址"，Base URL 填发不一（带不带 `/v1`、
    有没有粘上完整 endpoint）是 404 的头号原因，这样一眼就能看出来；
  - 「测试连接」按钮一次分清地址错 / Key 错 / 模型名错 / 网络不通，不用连设备；
- 扫描、选择、连接和断开 ESP-Claw BLE bridge；
- 获取设备信息，作为上位机 LLM 上下文；
- 「查看模块选择」只跑第一阶段路由，显示会导入哪些 README（不连设备也能用）；
- 输入自然语言需求并生成 Lua；
- 编辑、打开、保存 Lua，并上传到设备执行，执行日志区显示设备返回的真实结果；
- 「生成并自动迭代」按设备报错自动改脚本，轮数和等待秒数可在界面上调；
- 生成期间可点「停止生成」取消在飞的请求，不再只能干等；
- 查询或停止设备端 Lua job，查看执行日志；
- 后台任务并发执行：生成没结束时照样能停 job、查状态，不再被串行队列挡住。

## 故障与重试行为

新版把请求返回体和降级过程都报给界面，意在让失败**可诊断**而不是"请求失败"四个字：

- 非 2xx 会把服务端的响应体带进报错（`unsupported_parameter` 之类的原因全在 body 里）；
- HTTP 200 之后服务在流中间推的 `error` 帧会当成失败，不会最后报成"返回为空"；
- 429 / 5xx 等瞬时故障按 `Retry-After` 做有限重试；401 / 403 不重试（改字段治不好），
  而且始终保留**最初**那条错误，不拿第二次的失败去覆盖它；
- 服务因为某个字段拒收请求时，会读 body 精确改那个字段再重试
  （例如 OpenAI 新模型只认 `max_completion_tokens`、vLLM/Ollama 不认 `stream_options`）；
- 流式下超时是"多久没有新字节"，不再跟着 `max_tokens` 放大；

## LLM 接口风格

预设里两种接口风格都覆盖了，切换预设会连带切接口风格：

| 预设 | 接口风格 | 鉴权头 | 端点 |
|------|----------|--------|------|
| DeepSeek / Kimi / 百炼 / OpenAI / OpenAI 兼容 / 本地 vLLM/Ollama | `openai_chat` | `Authorization: Bearer` | `/chat/completions` |
| Anthropic 原生 | `anthropic` | `x-api-key` + `anthropic-version` | `/v1/messages` |

Anthropic 原生路径会额外处理：`system` 作为独立字段（不从 messages 里混过）、
连续同 role 的消息自动合并（Anthropic 不接受连续两条 user）、
`thinking` / `message_delta` 事件与 `stop_reason` 映射。

## 设备端协议

`ble_host_bridge` 支持的 op：`hello`、`device_info`、`put_lua`、`run_lua`、
`job_status`、`job_tail`、`job_stop`。`hello` 的返回里带有 `ops` 和 `pushes` 列表，
可以据此判断固件版本。

`job_tail` 用于增量取日志（`job_status` 只带 2048 字节尾巴）。job 进入终态时，
设备还会主动推一帧 `type=job_event` 的通知，上位机收到后记录下来；
轮询仍然是主路径，所以旧固件也能正常工作。

### Job 停止优化

GUI 界面的"停止 job"功能已优化：

- 设备端等待时间从 3 秒缩短到 1.5 秒
- 上位机请求超时从 20 秒缩短到 5 秒
- 添加设备事件监听，job 状态变化时立即显示
- 停止后自动查询一次状态确认

测试脚本：
```bash
python test_job_stop.py --device AA:BB:CC:DD:EE:FF
```

详细说明见 [STOP_JOB_FIX.md](STOP_JOB_FIX.md)。

## 测试

```bash
python test_host_vibe.py        # 或 pytest test_host_vibe.py
```

用假 bridge 和固定 LLM 返回覆盖迭代循环的停止条件，不需要真设备。

GUI 使用 Python 自带的 Tkinter，不新增第三方 GUI 依赖。BLE 和 LLM 网络操作运行在后台线程，界面不会被请求阻塞。电脑端配置和历史默认保存在 `~/.esp_claw/host_vibe_coding/`，不会写入设备端 NVS。

## 应用管理整合 · 2026-10-15

本地 `codex/ble-live-apps` 已整合 GitLab b9332b3c 的应用包、图标、SD 导出和 BLE
安装／下载／删除入口，并补齐设备端存储、动态首页和长按重命名。只整合应用管理；
当前分支语音协议保留，不引入该 GitLab 提交未交齐的音频事件接口。

两版电脑 GUI 的 Lua 编辑区新增“安装到应用栏 / 管理应用”：可设置 ID、名称、边框
和标签颜色、两态图标；新增、导出、列出、查看详情、读取代码和图标、删除及重命名。
图标可使用 PNG/JPEG/BMP 或原始 BIN；只选一张会生成暗色版本。不选择图片时使用默认图标。

```sh
python host_vibe.py export-app app.lua --app-id my_app --title 我的应用 --apps-dir /Volumes/CARD/apps
python host_vibe.py --device DEVICE install-app app.lua --app-id my_app --title 我的应用 --color 35C7A5 --pill-color 23866D
python host_vibe.py --device DEVICE list-apps
python host_vibe.py --device DEVICE app-info my_app
python host_vibe.py --device DEVICE download-app my_app --apps-dir ./backup/apps
python host_vibe.py --device DEVICE rename-app my_app 新名称
python host_vibe.py --device DEVICE remove-app my_app
python host_vibe.py --device DEVICE reload-apps
```

新包位于 DATA 的 `apps/gXXXXXXXX/<id>/app.json`，每组最多十项；ID 不覆盖，升级须
明确删除旧版后再安装。安装前检查固件 hello 能力，按1024字节分块写入，校验文件
大小、SHA-256 和图标格式后发布；取消或断传不发布半包。提交超时不代表失败，先查列表。
下载也校验每块身份、长度、EOF 和最终 SHA，仅读取，不执行代码。

设备安装后返回首页自动刷新，重启从索引恢复；长按菜单可查看、卸载（二次确认）、
重命名新格式用户应用。系统应用不可删除／重命名；旧 skills 格式继续由原入口管理。
正在运行或排队的应用不能从电脑端删除，应先停止；原生页面／Lua 在前台时延迟刷新。
中文名称可从电脑修改，设备虚拟键盘提供拉丁字符输入。

单脚本最多64KiB，图标为256×256 RGB565；协议只打包入口脚本和两张图标。
多文件 Lua、字体、海报、歌词及音频需另行部署。板型构建、宿主回归与实机验收分别记录，
详见[存储与验证说明](../../components/common/local_app_store/README.md)。

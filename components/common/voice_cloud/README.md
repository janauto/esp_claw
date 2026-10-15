# voice_cloud · 底座模式语音链路

更新日期：2026-10-16。集成契约见 `docs/backclip-1.0/CONTRACT.md` 第 3 节；验证记录见 [VALIDATION.md](VALIDATION.md)。

背夹在「底座·Wi-Fi」模式下自己完成语音链路：

```text
背夹录音 Opus 包（16 kHz 单声道 60 ms，每包约 100–250 B）── voice_cloud_push_opus()（任意任务，只拷贝）
   │  PSRAM 收包缓存（≤2000 包 / 512 KB）
   ▼  voice_cloud_finish() → 工作任务
Ogg Opus 封装（与语音中继 ogg_opus.py 逐字节一致）→ base64 → data URI
   │  POST <asr_base>/compatible-mode/v1/chat/completions（system 热词“背夹…” + input_audio）
   ▼  百炼 qwen3-asr-flash                                   事件：RECOGNIZING → HEARD(文字)
OpenAI 兼容对话（默认 DeepSeek deepseek-flash，关闭思考）+ 工具（最多 2 轮）
   │  工具回调在工作任务里执行                                事件：THINKING → TOOL(名, 参数)…
   ▼
回复清洗（去 Markdown/表情、换成设备字库里有的字，≤300 字节）   事件：REPLY(文字) 或 ERROR(中文短句, 错误码)
```

## 文件

| 路径 | 内容 |
| --- | --- |
| `include/voice_cloud.h` | 公开接口（契约签名原样保留，另加两个辅助函数） |
| `src/vc_ogg.c` | 收包格式迭代、Ogg CRC（常量表）、Ogg Opus 流式封装、流式 base64 |
| `src/vc_text.c` | UTF-8 工具、回复/识别文字清洗与截断 |
| `src/vc_proto.c` | 地址校验与拼接、ASR/对话请求 JSON、响应与工具调用解析、默认工具、错误文案 |
| `src/vc_pipeline.c` | 一次会话的流水线（ASR → 对话 → 工具 → 回复），HTTP/时钟/取消全部注入，与平台无关 |
| `src/voice_cloud.c` | 设备胶水：NVS 配置、收包缓存、会话与工作任务、事件回调 |
| `src/vc_http_esp.c` | 设备 HTTPS 传输：`esp_http_client` + `esp_crt_bundle_attach` + http_reuse 连接池 |
| `src/vc_internal.h` | 内部接口与全部常量（时限、上限、默认值、热词、错误文案） |
| `test_host/` | 宿主测试：`test_voice_cloud.c`（821 项检查）、`stubs/`（FreeRTOS/NVS/heap_caps 等桩）、`run_tests.py` |
| `tools/` | 真实云端核验：`vc_live.c`（公开 API + libcurl）、`live_check.py`（合成语音 → 全链路 → 结果汇总） |

纯逻辑四个文件不依赖 ESP-IDF；`voice_cloud.c` 在宿主上用 `test_host/stubs` 的 pthread/内存 NVS 桩编译，因此公开 API 的线程与状态逻辑也在宿主上跑过（ASan/UBSan/TSan）。只有 `vc_http_esp.c` 是设备专用。

## 构建依赖

`CMakeLists.txt`：

```cmake
REQUIRES cjson                       # 公开头文件包含 cJSON.h
PRIV_REQUIRES esp_http_client mbedtls nvs_flash esp_timer freertos heap log
```

- **IDF 6.1 没有 `json` 组件**（`~/esp/idf-mosaico-v6.1/components/json` 不存在），cJSON 来自托管组件 `espressif/cjson`，组件名写 `cjson`；`idf_component.yml` 声明 `espressif/cjson: "^1.7.19"`，与 `local_app_store`、`live_apps` 的做法相同。
- `esp_crt_bundle.h` 在 `mbedtls` 组件里；没有直接用 `esp-tls` 的头文件，所以不需要列它。
- 把组件加进工程：在 `application/edge_agent/main/idf_component.yml` 的 `dependencies` 下加

  ```yaml
  voice_cloud:
    path: ../../../components/common/voice_cloud
  ```

  并在使用它的组件里 `REQUIRES voice_cloud`（或 `PRIV_REQUIRES`）。组件管理器可能会因此更新 `dependencies.lock`。
- 可选编译宏（`target_compile_definitions(${COMPONENT_LIB} PRIVATE …)` 覆盖）：`VOICE_CLOUD_TASK_STACK`（默认 10240 字节）、`VOICE_CLOUD_TASK_PRIO`（默认 5）、`VOICE_CLOUD_TASK_STACK_PSRAM`（默认 0，见“内存与任务”）。

## 接口与语义

签名与契约一致；新增 `voice_cloud_default_tools_json()`（契约四个工具的 tools JSON）与 `voice_cloud_history_clear()`。

| 函数 | 说明 |
| --- | --- |
| `voice_cloud_init()` | 读 NVS 命名空间 `vcloud`，可重复调用（每次重新加载）。须在 `nvs_flash_init()` 之后；NVS 未初始化时返回对应错误 |
| `voice_cloud_asr_ready()` / `voice_cloud_chat_ready()` | 对应密钥已配置且地址可用 |
| `voice_cloud_config_set(fields)` | 见“配置”。先整体校验，任何字段非法则返回 `ESP_ERR_INVALID_ARG` 且什么都不写 |
| `voice_cloud_config_public()` | 返回新对象，调用者 `cJSON_Delete`。只有 `asr_ready/chat_ready/asr_model/chat_model/asr_host/chat_host` 六个字段，不含密钥 |
| `voice_cloud_set_tools(json, cb, ctx)` | OpenAI `tools` 数组（≤16 个、名字 `[A-Za-z0-9_-]{1,64}` 且不重名）；有工具时 `cb` 必填；`NULL`/`""` 清除工具。已在运行的会话用的是启动时的快照 |
| `voice_cloud_begin()` | 新开一段录音（PSRAM 一次分配 512 KB），并**取消正在进行的会话**（它不再回调） |
| `voice_cloud_push_opus(pkt, len)` | 只做一次短临界区内的拷贝，不分配、不等待。返回：`ESP_OK`；`ESP_ERR_INVALID_ARG`（空/0 字节/>1275 字节）；`ESP_ERR_INVALID_STATE`（没有 begin 或已 finish）；`ESP_ERR_INVALID_SIZE`（已满 2000 包=120 s 或 512 KB，本包丢弃并计数）。**仅限任务上下文，不能在 ISR 里调用** |
| `voice_cloud_finish(cb, ctx)` | 立即返回，见下表 |
| `voice_cloud_ask_text(text, cb, ctx)` | 跳过 ASR（会先发 `HEARD(text)`，便于首页统一显示“你说”），只走对话。文字需为合法 UTF-8，去首尾空白后不能为空，超过 1000 字节按字符边界截断 |
| `voice_cloud_cancel()` | 停止收包并丢弃录音、取消活动会话；之后不再回调（见“取消”） |
| `voice_cloud_busy()` | 有未取消的会话在处理 |
| `voice_cloud_packet_count()` | 正在录音时为已收包数；finish 之后为交给工作任务的包数；cancel 后为 0 |

`finish` / `ask_text` 的返回约定：**返回 `ESP_OK` ⇔ 回调一定会以一个终态（REPLY 或 ERROR）结束（除非随后被取消）；返回其他值 ⇔ 不会回调。**

| 情形 | 返回 | 回调 |
| --- | --- | --- |
| `cb == NULL` / 文字非法 | `ESP_ERR_INVALID_ARG` | 无 |
| 未 init；`finish` 前没有 `begin` | `ESP_ERR_INVALID_STATE` | 无 |
| 有会话在处理（忙） | `ESP_ERR_INVALID_STATE`，**录音保留且可继续收包**，空闲后可再 `finish` | 无 |
| 未配置（`finish` 需 ASR 与对话都就绪，`ask_text` 需对话就绪） | `ESP_OK` | 在**调用方任务里同步**回调 `ERROR("底座模式未配置 AI 服务", "not_configured")` |
| 录音少于 3 包（180 ms） | `ESP_OK` | 同步回调 `ERROR("没有录到声音，请再说一次", "no_audio")` |
| 建任务或快照内存不足 | `ESP_ERR_NO_MEM` | 无 |
| 正常 | `ESP_OK` | 工作任务里依次回调 |

事件顺序：`RECOGNIZING → HEARD(识别文字) → THINKING → [TOOL(工具名, 参数 JSON)…] → REPLY(回复)`；任一步失败以 `ERROR(中文短句, 错误码)` 结束。`text/detail` 永不为 `NULL`（无内容时为 `""`），只在回调期间有效。**终态回调发出时 `voice_cloud_busy()` 已为 false**，可以在回调里直接开始下一句。

对应契约 `assistant_show`：RECOGNIZING→`recognizing`，HEARD→`heard`，THINKING→`thinking`，REPLY→`reply`，ERROR→`error`；TOOL 可显示为“正在调亮度…”之类，也可忽略。

### 取消与并发

- 同一时刻只有一个活动会话；被取消的会话在当前 HTTP 请求返回后自行收尾、释放内存。为了让“取消后马上再说一句”不被拒，允许“一个收尾中的已取消会话 + 一个新会话”同时存在（最多 2 个工作任务），再多则返回 `ESP_ERR_INVALID_STATE`。
- 取消是协作式的：在每次请求前后、退避等待中、工具执行前检查；**进行中的 `esp_http_client_perform` 无法被打断**，最长等到这次请求的超时。
- `cancel()`/`begin()` 返回后，被取消的会话不再发终态；但若取消恰好发生在一个非终态回调正在执行时，那个回调仍会执行完。会话已产生终态（已清除忙状态）后再 `cancel()` 不影响该终态回调。建议 `user_ctx` 指向长期有效的对象。

## 配置（NVS 命名空间 `vcloud`）

| 键 | 默认值 | 校验与规范化 |
| --- | --- | --- |
| `asr_base` | `https://dashscope.aliyuncs.com` | 只接受 `https://`；不得含用户名、`?`、`#`、空白；去掉末尾 `/`，也去掉误填的 `/compatible-mode/v1[/chat/completions]` |
| `asr_key` | — | 可见 ASCII，1–256 字节；首尾空白（含换行）去掉 |
| `asr_model` | `qwen3-asr-flash` | 可见 ASCII，≤64 字节 |
| `chat_base` | `https://api.deepseek.com` | 同 `asr_base` 规则（保留路径，如 `/v1`；末尾 `/chat/completions` 会被去掉） |
| `chat_key` | — | 同 `asr_key` |
| `chat_model` | `deepseek-flash` | 同 `asr_model` |

- `""` 或 `null` = 清除（删 NVS 键，回到默认）；未知字段忽略；非字符串值报错。
- `asr_ready` = 有 `asr_key`；`chat_ready` = 有 `chat_key`（根地址都有默认值）。`config_public` 的 `*_host` 是实际会访问的主机名（含默认值）。
- 对话地址：`chat_base` 含 `/compatible-mode/` → `<base>/chat/completions`；主机含 `dashscope` 或与 `asr_base` 同主机（业务空间专属域名）→ `<base>/compatible-mode/v1/chat/completions`；否则 `<base>/chat/completions`。
- 思考参数：百炼路线带 `"enable_thinking":false`；主机含 `deepseek` 或模型名以 `deepseek` 开头带 `"thinking":{"type":"disabled"}`；其他 OpenAI 兼容服务不带。
- NVS 未加密：读出 Flash 就能拿到密钥。若需要防物理提取，请启用 NVS 加密（本组件不改分区与 NVS 配置）。

## 请求格式

ASR（与 `relay/voice_relay.py` 的 `transcribe()` 相同）：

```json
{"model":"qwen3-asr-flash",
 "messages":[{"role":"system","content":[{"type":"text","text":"背夹 磁吸背夹 品胜 小磁 底座 亮度 音量 Lua LVGL ESP-Claw WorkBuddy"}]},
             {"role":"user","content":[{"type":"input_audio","input_audio":{"data":"data:audio/ogg;base64,T2dnUw…"}}]}],
 "stream":false,"asr_options":{"enable_itn":true,"language":"zh"}}
```

- Ogg：第 0 页 OpusHead（版本 1、单声道、pre-skip 312、输入采样率 16000，BOS），第 1 页 OpusTags（厂商串 `backclip-voice-cloud`），之后每 16 包一页，最后一页 EOS；granule 按 48 kHz，从 312 起每包 +2880；序列号 `0x42435650`。单包很大导致一页 lacing 超过 255 时会自动拆页（正常 ≤1275 字节的包不会触发）。
- 正文只分配一次：先用 cJSON 生成带占位符的骨架（负责转义），再把 base64 直接流式写进最终缓冲，大块音频不经过 cJSON。正文建好后立即释放收包缓存。
- 热词上下文必须带：不带时实测把“背夹”识别成“薇家”（旧记录为“背佳”），见 VALIDATION.md。

对话（与手机端 `chat_agent.dart` 相同的循环）：

- `messages` = 系统提示（中文、口语、≤80 字、不用 Markdown/表情；明确要求才操作设备、必须调用工具；当前本地时间——仅当系统时钟已同步到 2024 年以后）+ 最近 4 轮历史 + 本句。
- 带 `tools` 时：第 1、2 轮 `tool_choice:"auto"`；模型返回 `tool_calls` 就把助手消息（含原样 `tool_calls`，若有 `reasoning_content` 也带回）追加进去，按顺序执行每个调用并各追加一条 `{"role":"tool","tool_call_id":…,"content":…}`；第 3 轮仍带 `tools` 但 `tool_choice:"none"`，逼模型给出文字。第 3 轮仍无文字时，若已执行过工具则回复“好的，已经办好了。”，否则报 `chat_empty`。
- 工具结果（tool 消息内容）：成功 `{"ok":true,"result":<回调 out：合法 JSON 对象/数组原样，否则字符串，空则 "done">}`；回调返回错误 `{"ok":false,"error":"ESP_ERR_…","message":<out>}`；未声明的工具名 `unknown_tool`、参数不是 JSON 对象 `invalid_arguments`、同一轮第 5 个起 `too_many_calls`（都不执行）。参数可为 JSON 字符串或直接是对象；空串视为 `{}`。
- 历史只记成功会话的“用户原话（≤600 字节）/清洗后的回复”，被取消或失败的会话不记。

## 重试与时限

| 项 | 值 |
| --- | --- |
| ASR 阶段 | 15 s + 录音时长的 1/8，最多 30 s（120 s 录音给 30 s） |
| 对话阶段（含所有轮次、工具执行与重试） | 25 s |
| 单次请求网络超时 | min(阶段剩余, 15 s) |
| 重试 | 429、5xx、正文含 `too busy`、连接失败、网络超时：最多 2 次，退避 600 ms、1200 ms；剩余时间不足（< 退避 + 1.5 s）就不再重试，返回上一次的错误 |
| 不重试 | 400/401/402/403/404 等、响应过大、内存不足 |
| 响应上限 | ASR 16 KB，对话 48 KB（PSRAM 按需增长） |

## 错误码（ERROR 事件的 detail）

| detail | text（设备字库全部可显示，已逐字核对） |
| --- | --- |
| `not_configured` | 底座模式未配置 AI 服务 |
| `no_audio` | 没有录到声音，请再说一次 |
| `no_mem` | 内存不足，请稍后再试 |
| `asr_net` / `chat_net` | 网络连接失败，请检查 Wi-Fi |
| `asr_timeout` / `chat_timeout` | 服务响应超时，请再试一次 |
| `asr_bad_response` / `chat_bad_response` | 服务返回异常，请稍后再试 |
| `asr_empty` | 没听清，请再说一次 |
| `chat_empty` | 没有得到回复，请再试一次 |
| `asr_http_<码>` / `chat_http_<码>` | 401/403：语音识别（对话）服务鉴权失败，请检查密钥；402：对话服务账户余额不足；404：服务地址或模型名不对，请检查配置；429：服务太忙了，请稍后再试；5xx：服务暂时不可用，请稍后再试；400：语音识别请求被拒绝，请再说一次 / 对话请求被拒绝，请换个说法；其他：服务出错，请稍后再试 |

服务端返回的错误正文（可能带打码的密钥）一律不记录、不外传。

## 回复清洗与设备字库

设备字体 `fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf` 共 3680 个字形。用 fontTools 逐一核对 cmap 与字形轮廓：ASCII 只缺 `!` 和 `^`；常用中文标点 `，。！？：；、（）《》【】「」『』“”‘’…—·～￥` 都有且有轮廓；`° → ℃ ％ ＋ × ÷` 及表情、方块、箭头都没有。

> 与任务说明的差异：说明里举例要替换 `· … “ ”`，但字库里这四个字有真实字形，所以**保留不换**（内存笔记“没有 · …”与字体文件不符，以字体文件为准）。`!` `^` `°` `→` 按说明处理。如需更保守，改 `vc_text.c` 的 `put_mapped()` 一处即可。

| 输入 | 输出 |
| --- | --- |
| `**粗体**`、`*斜体*`、`` `代码` ``、`~~删除~~`、`__下划线__` | 去掉记号保留文字 |
| ```` ``` ```` 代码块 | 整段去掉 |
| 行首 `#`、`>`、`- `/`* `/`+ `/`• `、`1. `/`1) `/`1、`、`[ ]` | 去掉；多行用“，”（中文后）或空格（英文后）接成一行，已有句读不再补 |
| `[文字](链接)`、`![图](链接)` | 只留文字 |
| 分隔线、表格分隔行；`\|` | 去掉；`\|` 变空格 |
| `!` | `！`（全角，字库有） |
| `^` | 去掉 |
| `°C`/`℃`、`°F`/`℉`、单独 `°` | 度、华氏度、度 |
| `→ ⇒ ➡`、`← ⇐ ⬅` | `->`、`<-` |
| `× ÷ ± − – ≤ ≥ ≠ ≈` | `x / +- - - <= >= 不等于 约` |
| `¥ € £` | `￥ 欧元 英镑` |
| `①…⑳` | `1.`…`20.` |
| 字库外的全角 ASCII（如 `５０％`） | 换回半角（`50%`）；字库里有的 `！（），：；？～` 等保留 |
| 表情、杂项符号、方块、制表符、零宽字符、私用区、所有 U+10000 以上字符、控制字符、非法 UTF-8 | 去掉 |
| 连续空白 | 合并为一个空格，首尾去掉 |

最后按 UTF-8 截到 300 字节（“听到”为 240 字节）：句末（。！？；?）落在后 40% 内就在句末截断；否则硬截并补“…”（总长仍 ≤ 上限，绝不切坏多字节字符）。

## 内存与任务

- 收包缓存：`begin()` 时一次分配 512 KB PSRAM；正文建好后立即释放。
- ASR 正文 ≈ Ogg × 4/3 + 约 400 字节（实测 3.7 s 录音：62 包 → Ogg 10.6 KB → 正文 14.4 KB；120 s 上限约 0.5 MB）。峰值 ≈ 收包缓存 + 正文，全部在 PSRAM（`vc_big_malloc` 优先 `MALLOC_CAP_SPIRAM`）。
- 会话快照（配置、密钥副本、历史、工具 JSON）约 2 KB + 工具 JSON，结束时释放，密钥副本先清零。
- 工作任务：每个会话新建、结束自删（`xTaskCreatePinnedToCoreWithCaps` + `vTaskDeleteWithCaps(NULL)`），栈默认 **10 KB 放内部 RAM**。原因：工具回调可能写 NVS（例如 `set_mode` 持久化），本机 `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` 时 Flash 写入只有在仍有 Flash mmap 页时才关 Cache，但一旦关了，PSRAM 上的栈不可访问会直接异常。内部 RAM 不够时自动退回 PSRAM；若确认回调不写 Flash，可定义 `VOICE_CLOUD_TASK_STACK_PSRAM=1` 优先 PSRAM。每次会话结束日志会打印栈剩余（`stack_free`），上机后据此调整。
- HTTPS：`keep_alive_enable=true` 走 `components/common/http_reuse` 连接池，一次会话里 ASR、对话多轮复用 TLS 连接；空闲 60 s 后由连接池回收。TLS 缓冲按 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` 在 PSRAM。
- 代码体积（目标工具链 -Os）：text 约 29 KB，data 16 B，bss 100 B。

## 线程安全与调用时机

- `push_opus`：任意任务；自旋临界区里只做 ≤1275 字节拷贝（约十微秒量级），不分配内存。
- 其余接口：任意任务；内部用一把互斥量保护配置/工具/历史/会话状态，**任何用户回调都不在持锁时调用**，回调里调用本组件任何接口（包括 cancel、begin、ask_text）都不会死锁。
- 事件回调与工具回调都在工作任务里执行：请只做拷贝并投递到 UI 任务，不要在回调里长时间持有 LVGL 锁或做网络操作；工具回调要尽快返回（它占用对话阶段的 25 s）。
- 建议时序：`nvs_flash_init()` → `voice_cloud_init()` → `voice_cloud_set_tools(voice_cloud_default_tools_json(), 自己的分发函数, ctx)`；按住 M → `begin()`；录音任务每包 `push_opus()`；松开 → `finish(cb, ctx)`；底座模式切走或用户取消 → `cancel()`。BLE/UART 的 `ai_config_get`/`ai_config_set` 直接对接 `config_public`/`config_set`，`voice_text` 对接 `ask_text`。

## 安全

- 密钥只在 NVS、会话快照（结束清零）和 HTTP 头里；日志只记阶段、字节数、耗时、HTTP 状态码、错误码与工具名，不记识别文字、回复、请求正文或服务端错误正文。宿主测试在 `VC_LOG=1` 下检查日志不含测试密钥；云端核验检查所有输出与请求转储不含真实密钥。
- 只接受 HTTPS；关闭自动重定向和授权重试（Authorization 不会被带到别的主机）；连接归还连接池前删除 Authorization 头。

## 测试与核验

```sh
# 宿主测试（clang + ASan/UBSan、TSan、macOS leaks、Ogg 交叉核对、字库覆盖）
python3 components/common/voice_cloud/test_host/run_tests.py
# 可选：--ref-ogg <relay/ogg_opus.py>（默认 ~/.cache/backclip-1.0-refs/…/relay/ogg_opus.py）、--skip-tsan、--skip-leaks

# 真实云端核验（需要 say、ffmpeg(libopus)、ffprobe、libcurl；密钥读 ~/.config/backclip-voice/secrets.env）
python3 components/common/voice_cloud/tools/live_check.py --out /tmp/voice-cloud-live [--dashscope-chat qwen-plus]
```

`run_tests.py` 只在临时目录编译；cJSON 默认取 `application/edge_agent/managed_components/espressif__cjson/cJSON`（可用 `APP_TEST_CJSON` 覆盖）。

## 已知边界

- **未在目标板上构建链接、未上机**：已用 IDF 6.1 RISC-V 工具链按工程真实编译参数（-Wall -Werror，取自已有构建的 `compile_commands.json`）把 6 个源文件逐一编成目标文件，但组件尚未加入工程统一构建，Wi-Fi、TLS 握手内存、栈余量、连接池复用、真实麦克风 Opus 都待实机验证。
- 取消无法打断进行中的 HTTP 请求（见上）。
- 模型行为有随机性：实测提示词里写明“明确要求才操作设备、必须调用工具”后，DeepSeek 闲聊不再擅自打开应用（5/5），设备操作句都正确调用工具；但不能保证每次都如此。
- 回复只保证字库里的常用标点；生僻汉字若不在 3503 个常用字里仍会显示为方框。

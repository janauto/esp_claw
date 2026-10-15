# voice_cloud 验证记录

日期：2026-10-16（北京时间 00:20–00:55）。返回[说明](README.md)。

环境：macOS 27.0.1（Apple Silicon）、Apple clang 21.0.0、Python 3.9.6、ffmpeg/ffprobe 8.0.1（libopus）、curl/libcurl 8.7.1、
ESP-IDF v6.1（`~/esp/idf-mosaico-v6.1`）RISC-V 工具链 `riscv32-esp-elf-gcc` 15.2.0（esp-15.2.0_20251204）、cJSON 1.7.19（工程托管组件）。

结论：**宿主测试 10/10 项通过（单元检查 821 项，ASan/UBSan、TSan、leaks 全部干净）；真实云端核验 8/8 通过（百炼 qwen3-asr-flash + DeepSeek deepseek-flash，含两轮工具调用与百炼对话路线）；6 个源文件用 IDF 6.1 目标工具链按工程真实参数编译通过。没有在工程里统一构建、没有刷写、没有上机。**

## 分项结果

| 项目 | 类别 | 结果 | 证据与边界 |
| --- | --- | --- | --- |
| 单元测试（ASan + UBSan） | 主机测试 | 通过，821 项检查 0 失败 | `test_host/run_tests.py`；覆盖下文“宿主测试覆盖” |
| 单元测试（TSan） | 主机测试 | 通过，821 项 0 失败、无数据竞争告警 | 公开 API 的工作任务在宿主上是真实 pthread（`test_host/stubs`） |
| 泄漏 | 主机测试 | 通过 | macOS `leaks --atExit`：0 leaks / 0 bytes（测试最后 `vc_internal_reset()` 释放全部） |
| 日志不含密钥 | 主机测试 | 通过 | `VC_LOG=1` 下 151 行日志，不含测试密钥；日志只有阶段、字节数、耗时、状态码、错误码、工具名 |
| Ogg 与语音中继逐字节一致 | 主机测试 | 通过 4/4 + 真实 Opus 1/1 | 与 `relay/ogg_opus.py`（已与 libogg 对齐）比对：fake_packets(40/1/17)、600/255 字节 lacing、ffmpeg 编出的 51 个真实包 |
| CRC 与 libogg 一致 | 主机测试 | 通过 | ffmpeg(libogg) 写出的 4 页逐页复算 CRC 全对；常量查表与逐位算法 1000 字节随机数据一致 |
| ffprobe 读 C 封装 | 主机测试 | 通过 | 背夹同参数（16 kHz 单声道 24 kbps 60 ms voip）51 包 → opus、单声道、3.07 s（≈51×60 ms） |
| ASR 请求体 | 主机测试 | 通过 | C 生成的正文解析回来：模型、stream=false、热词 system、`asr_options`；data URI 解码后与 C 封装逐字节相同且 ffprobe 可读 |
| 字库覆盖 | 主机测试 | 通过 | 28 条给用户看的固定中文 + 清洗用到的替换字逐字在 3680 字形的设备字体里；云端核验的全部回复也无缺字 |
| 目标工具链编译 | 编译构建（单文件） | 通过 | 取已有构建 `compile_commands.json` 中 `live_apps/src/live_http.c` 的完整参数（含 `-Wall -Werror -Wextra`、`-Os`、`-std=gnu23`、sdkconfig），逐个编译 6 个源文件到临时目录，0 告警；text 29012 B、data 16 B、bss 100 B。没有改动或写入 `application/edge_agent/build` |
| 工程统一构建与链接 | 编译构建 | **未做** | 按要求留给主会话集成后统一构建 |
| 真实云端：语音 → ASR → 对话 → 工具 | 真实服务 | 通过 | 见下表；宿主驱动 `tools/vc_live.c` 走组件公开 API，只有 HTTP 换成 libcurl |
| 热词 A/B | 真实服务 | 符合预期 | 同一段语音：带热词“背夹，帮我把亮度调到50%。”；不带热词“**薇家**，帮我把亮度调到50%。” |
| 输出不含密钥 | 真实服务 | 通过 | 25 个输出流/请求转储逐一检查 3 个真实密钥值均未出现（检查脚本读取了值但不打印、不落盘） |
| 刷写与实机 | 实机验收 | **未做** | 未连接背夹；Wi-Fi、mbedTLS 内存、栈余量、连接池复用、真实麦克风 Opus 都待实机 |

## 宿主测试覆盖（test_host/test_voice_cloud.c）

- base64：RFC 4648 向量；0–301 字节任意分段流式编码 = 一次编码 = 可解回；输出缓冲不足报溢出。
- 收包格式：截断长度前缀、零长度包、长度越界都报错。
- Ogg：页数、页类型（BOS/EOS）、序号、序列号、granule（312 + n×2880）、CRC、OpusHead/OpusTags 字段、拆包还原；600/255 字节 lacing；17 个 4000 字节大包触发自动拆页且仍合法；`vc_ogg_size` 与实际字节数相等；零包拒绝。
- ASR 请求：结构、热词、data URI 解码一致；不带热词时只有 user 消息；ASR 响应解析（字符串/分段数组/null/各种畸形）。
- 地址：默认根、业务空间域名、多余路径与斜杠、`http://`/用户名/查询串/片段/空白/非法字符拒绝；对话地址 DeepSeek（含 `/v1`、已带 `/chat/completions`）、百炼（dashscope 域名、与 ASR 同主机的业务空间域名、已带 `/compatible-mode/v1`）、普通 OpenAI 兼容；`thinking`/`enable_thinking` 按路线添加。
- 对话请求：系统提示（≤80 字、当前时间）、4 轮历史顺序、tools/tool_choice、只引用不接管 messages/tools。
- 工具定义校验：非法 JSON、非数组、空数组、缺名字、名字非法、类型不是 function、重名、parameters 非对象、超过 16 个。
- 文字清洗：32 个用例（Markdown、列表、代码块、链接/图片、表格、`!`/`^`/°C/℃/→/全角数字/¥/×÷、表情、零宽、控制字符、非法 UTF-8、①②、换行拼接）；300 字节截断补“…”、句末截断；0–139 每个上限下输出都是合法 UTF-8 且不超长。
- 流水线（假传输 + 虚拟时钟）：语音 → ASR → 工具 → 回复全流程与请求内容（工具消息、tool_call_id、思考关闭、密钥只进对应请求）；两轮工具后第 3 轮 `tool_choice:none`；第 3 轮仍要调工具时的收尾；未知工具/坏参数/回调失败/每轮第 5 个调用；503×2 后成功（退避 600+1200 ms）、503×3、429、401 不重试、400 正文 too busy 重试、网络错误、响应过大、内存不足、坏 JSON、空回复、ASR 500/403/坏响应/空文字；对话 25 s 时限（第 2 次只给剩余 9.4 s）、ASR 时限内来不及重试时返回 503 而非超时；ASR 中/工具执行中/退避中取消。
- 公开 API（pthread 桩）：未 init 时各接口；未配置时 finish/ask_text 在调用方线程同步回调 `not_configured`；配置校验失败不写任何东西、规范化后落 NVS、`config_public` 不含密钥且只有 6 个字段、重新 init 后从 NVS 恢复、`""`/null 清除、NVS 写失败时内存配置不变；push 的参数检查、2000 包上限、512 KB 字节上限（1275 字节包恰好 410 个）；完整语音会话的事件顺序、终态回调时 `busy()` 已为 false、回调在工作线程；忙时 ask_text/finish 返回 `INVALID_STATE` 且录音保留可继续收包；begin 取消进行中的会话且被取消的会话不再回调；cancel 后无回调；历史只保留 4 轮、`history_clear`；清除工具后请求不带 tools；建任务失败返回 `ESP_ERR_NO_MEM` 且无回调无残留；录音过短 `no_audio`。

## 真实云端核验（tools/live_check.py，最终一轮 live5）

语音由 `say -v Tingting` 合成，ffmpeg/libopus 编成 16 kHz 单声道 24 kbps 60 ms（voip）后按包调用 `voice_cloud_push_opus()`，再 `voice_cloud_finish()`。时间从 `finish()` 起算；ASR 与对话请求的 TLS 都是宿主 libcurl（LibreSSL），设备上是 mbedTLS + 连接池，耗时会不同。工具由核验驱动里的假执行器回 `{"brightness":50}` 之类结果。

| 用例 | 录音 | ASR 请求 | ASR 耗时 | 对话耗时（各轮） | 总耗时 | 识别（HEARD） | 工具调用（TOOL） | 回复（REPLY） |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 调亮度 | 3.72 s / 62 包（38–232 B，均 166 B） | Ogg 10573 B → 正文 14442 B | 565 ms | 563 + 824 ms | **1978 ms** | 背夹，帮我把亮度调到50%。 | `set_brightness {"percent":50}` | 亮度已经调到50%了。 |
| 音量 + 模式 | 5.82 s / 97 包 | Ogg 16657 B → 22554 B | 694 ms | 1129 + 859 ms | 2700 ms | 背夹，把音量调到30%，然后切换到底座模式。 | `set_volume {"percent":30}`、`set_mode {"mode":"dock"}` | 音量已调到30%，也切换到 底座模式了。 |
| 打开应用 | 1.68 s / 28 包 | Ogg 4666 B → 6566 B | 512 ms | 960 + 855 ms | 2344 ms | 打开天气应用。 | `open_app {"name":"天气"}` | 天气应用已经打开了。 |
| 闲聊 | 2.40 s / 40 包 | Ogg 6951 B → 9610 B | 513 ms | 1082 ms | 1610 ms | 今天适合出去跑步吗？ | 无 | 我这边看不到天气数据，你打开天气应用看看，或者告诉我你在哪个城市，我帮你查。 |
| 文字（ask_text） | — | — | — | 729 ms | 734 ms | 背夹，给我讲一个很短的笑话 | 无 | （一则约 45 字的笑话） |
| 百炼对话路线（qwen-plus） | 3.72 s / 62 包 | 14442 B | 627 ms | 881 + 650 ms | 2171 ms | 背夹，帮我把亮度调到50%。 | `set_brightness {"percent":50}` | 亮度已调到50% |

- 请求核对：对话请求为 `https://api.deepseek.com/chat/completions`，带 4 个工具、`tool_choice:"auto"`、`"thinking":{"type":"disabled"}`，系统提示含当前时间；第二轮带回原样 `tool_calls`（DeepSeek 返回的 id 形如 `call_00_…`）与对应 tool 消息；百炼路线为 `<业务空间域名>/compatible-mode/v1/chat/completions`、`"enable_thinking":false`、不带 `thinking`。ASR 请求转储里的 data URI 解码后 ffprobe 均为 opus、时长 = 包数 × 60 ms。
- 多轮稳定性：live3、live4、live5 三轮全部 8/8；DeepSeek 路线各用例总耗时 1.4–3.7 s，ASR 0.51–1.21 s（1.7–5.8 s 录音）。核验期间没有遇到 DeepSeek 503，重试路径只在宿主测试里用假传输验证过。
- 提示词调整（均有实测依据）：live2 中百炼 qwen-plus 只回“正在把亮度调到50%”而未调用工具、DeepSeek 在闲聊里擅自 `open_app(天气)`；于是在系统提示里写明“明确要求操作设备时必须调用对应工具，没有提出时不要擅自操作”，并在 `open_app` 的描述里写明“只在用户明确要求打开某个应用时调用”。之后同一闲聊语音连跑 5 次均未调用工具（总耗时 1500–1833 ms，回复都改为“要我帮你打开吗？”），设备操作句仍全部正确调用工具。
- 首次手动文字核验（“背夹，把亮度调到百分之五十”）：DeepSeek 两轮 902 + 869 ms，`set_brightness {"percent":50}` → “亮度已调到百分之五十。”。

## 待实机验证（按顺序）

1. 主会话把组件加进 `main/idf_component.yml` 后统一构建；确认 `cjson` 依赖解析、链接无重复符号、http_reuse 的 `--wrap` 生效（日志出现 `reuse hit`）。
2. 上机：`voice_cloud_init()` 后用 BLE/UART `ai_config_set` 配置，`ai_config_get` 不回显密钥；重启后仍就绪。
3. 底座模式按住 M 说“背夹，把亮度调到百分之五十”：首页依次显示识别中 / 你说 / 思考 / 回复，亮度工具被调用；记录 `session end … stack_free=` 与各阶段日志耗时，栈剩余不足 2 KB 时调大 `VOICE_CLOUD_TASK_STACK`。
4. 断 Wi-Fi、错密钥、错模型名分别得到 `asr_net`/`asr_http_401`/`chat_http_404` 类错误与中文提示。
5. 录音中再次按 M、说话中切模式（cancel）：不残留忙状态，下一句正常。
6. 连续 10 句：PSRAM/内部 RAM 不持续下降（每句结束释放）；连接池复用后第二句的 TLS 握手耗时下降。
7. 工具回调里写 NVS（`set_mode`）时栈在内部 RAM，不出现 Cache 访问异常。

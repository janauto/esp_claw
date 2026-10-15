# 磁吸背夹固件 1.0 · 集成契约（各模块共同遵守）

日期：2026-10-15。底座：`codex/ble-live-apps` @ dbd67483（BLE NimBLE + 实时应用 + 应用包管理 + BLE 语音上行）。
1.0 分支：`release/backclip-1.0`（本仓库，本地，不推 GitLab）。

## 0. 产品结构

- **系统首页（原生 C，system_ui）**：按《首页设计原型 v0.1》（HomeView 风格）——状态栏/软键栏是系统外框；小组件页（最多 3 页）+ 第 1 页底部应用栏（≤4）；上滑应用总览；下拉控制中心（含两种模式）；长按编辑；按住 M 说话。
- **「我的」= Lua 客制化花园**（SYSTEM 内置 Lua 应用 `my_garden`）：小磁形象、小家（主题外壳）、花园地块（沙箱化的用户 Lua“植物”）、主题切换与 AI 生成主题/植物。
- **两种模式**（控制中心切换，NVS 持久化）：
  - `clip` 背夹·手机：不插电，语音经 BLE 送手机，手机 ASR + DeepSeek，回复回显到背夹；
  - `dock` 底座·Wi-Fi：背夹自己经 Wi-Fi 直连云端 ASR（百炼 qwen3-asr-flash）+ 对话模型，自主完成语音链路。

## 1. 路径约定（设备）

| 用途 | 路径 |
| --- | --- |
| 系统只读根 | `<SYSTEM>`（固件 FAT 镜像，源码 `application/edge_agent/fatfs_image/system/`） |
| 数据根 | `<DATA>`（样机为 `/sdcard`） |
| 花园程序 | `<SYSTEM>/skills/my_garden/`：`SKILL.md`、`scripts/main.lua` 及模块 |
| 花园数据 | `<DATA>/garden/`：`garden.json`（布局与植物状态）、`plants/*.lua`（用户/AI 植物）、`themes/*.lua`、`snapshot.json` |
| 首页布局 | `<DATA>/home/layout.json` |

### 1.1 花园启动与退出

- 原生首页点「我的」小组件或总览「我的 · 小磁」→ 以首页启动应用同一路径启动：`cap_lua_run_script_async("<SYSTEM>/skills/my_garden/scripts/main.lua", args_json, 0, "my_garden", "display", true, ...)`，独占显示。
- `args_json`：`{"page":"mine"}`（默认）或 `{"page":"house"}` / `{"page":"garden"}`。
- 退出：Lua 主循环结束即回原生首页。约定 SET 短按 = 返回上一级，顶层再按 SET = 退出；系统底部上滑也会停止应用（固件已有）。

### 1.2 snapshot.json（花园 → 原生首页「我的」小组件）

花园在状态变化/退出时写入（先写 `.tmp` 再 rename；FAT 不能覆盖式 rename 时先删旧文件）：

```json
{"v":1,"pet_name":"小磁","mood":"开心","mood_level":8,"energy":72,"days":23,
 "theme":"house","theme_name":"小家","say":"欢迎回家","plants":6,"updated":1792080000}
```

原生首页每次回到首页时读一次；文件缺失/损坏时显示默认“小磁 · 点我进入我的花园”。

## 2. BLE / UART 操作（1.0 新增或扩展）

BLE：沿用 RX `7b7d1002` JSON 请求、TX `7b7d1003` 响应（`{"id","ok","data"|"error"}`）。UART：`clip_cmd <JSON>` → `CLIP_RESULT {...}`（同一处理函数，`ok` + 字段平铺）。`hello` 的 `ops` 必须列出全部新操作，并带 `"release":"1.0.0"`、`"mode"`。

| op | 请求 | 返回（data） |
| --- | --- | --- |
| `mode_get` | — | `{"mode":"clip"|"dock","voice_route":"ble"|"wifi","phone_linked":bool,"wifi":{"connected":bool,"ssid":"…"},"ai_ready":bool}` |
| `mode_set` | `{"mode":"clip"|"dock"}` | 同 `mode_get` |
| `ai_config_get` | — | `{"asr_ready":bool,"chat_ready":bool,"asr_model":"…","chat_model":"…","asr_host":"…","chat_host":"…"}`（**永不返回密钥**） |
| `ai_config_set` | 任意子集：`asr_base, asr_key, asr_model, chat_base, chat_key, chat_model`；空串=清除 | 同 `ai_config_get` |
| `apps_all` | — | `{"apps":[{"id","title","kind","group","ai","removable","renamable","in_dock","running"}]}` |
| `app_launch` | `{"id":"sys:…"|"skill:…"|"app:…"|"garden"}` | `{"launched":true}` |
| `app_remove` | `{"id"}`：`app:<pkg>`、`skill:<dir>`；**无前缀=包 ID（兼容 0.6.0 App）** | 原有格式 |
| `app_rename` | `{"id","title"}`，同上前缀规则 | 原有格式 |
| `home_get` | — | `{"layout":{…},"widgets":[{"w","name","sizes":["S","M","L"],"app"}],"dock_max":4}` |
| `home_set` | `{"layout":{…}}` | `{"layout":{…}}`（校验后生效并持久化） |
| `assistant_show` | `{"state":"listening|recognizing|thinking|reply|error|hide","heard":"…","text":"…"}` | `{}` |
| `garden_put` | `{"kind":"plant"|"theme","id":"小写字母开头，[a-z0-9_]≤32","source":"Lua 源码 ≤16KB"}` | `{"kind","id","saved":true}`；写入 `<DATA>/garden/plants|themes/<id>.lua`，花园几秒内热重载（2026-10-16 rc18 起） |
| `garden_list` | — | `{"plants":[id…],"themes":[id…]}` |
| `garden_remove` | `{"kind","id"}` | `{}` |
| `voice_intent` | `{"text":"识别文字"}` | `{"handled":bool,"reply":"确认语"}`；背夹认得的设备指令（调音量/亮度、切换模式、打开应用）当场执行，`handled=false` 时手机照常问对话模型（2026-10-16 rc11 起） |

ID 命名空间：`sys:<id>` 系统原生/固件内置（不可卸载/改名）；`skill:<dir>` 旧 SKILL.md 应用（DATA/skills）；`app:<id>` 应用包（DATA/apps）；`garden` 我的花园。`kind` ∈ `system|builtin_lua|skill|package|garden`；`group` ∈ `builtin|official|mine`（总览分组：内置应用/官方应用/我的创作）；`ai`=EPP（Lua 应用，显示 AI 角标）。

### 2.1 首页布局 JSON（`<DATA>/home/layout.json` 与 `home_get/home_set`）

```json
{"v":1,
 "pages":[[{"w":"mine","s":"S"},{"w":"clock","s":"S"},{"w":"weather","s":"M"}],
          [{"w":"stocks","s":"S"},{"w":"system","s":"S"},{"w":"aihot","s":"M"}]],
 "dock":["sys:meeting","sys:ir","sys:card","sys:workbuddy"]}
```

- 每页 2 列网格：S=1 格（半宽方块）、M=横跨 2 列 1 行、L=2×2；每页最多 4 个 S 的面积（2 列 × 2 行）。最多 3 页。
- `mine` 小组件固定存在（可换尺寸，不能移出）。dock ≤4，仅第 1 页显示。
- 损坏或校验失败 → 回退默认布局（不丢弃文件，另存 `.bad`）。

### 2.2 UART 专用测试操作（模拟用户输入，用于自动验收）

| op | 请求 | 说明 |
| --- | --- | --- |
| `ui_key` | `{"key":"MODE|SET|VOLUME_UP|VOLUME_DOWN","action":"click|long|press|release","ms":…}` | 走与实体键相同的路径 |
| `ui_touch` | `{"type":"tap|long|drag","x","y","x2","y2","ms"}` | 注入触摸（坐标为 480×800 竖屏） |
| `home_state` | — | 当前界面（home/overview/control/edit/voice/app）、页码、焦点、布局、语音状态；rc12 起另有 `volume`、`brightness`，总览时 `ov_focus`（焦点应用），菜单/弹窗时 `target`（针对的应用） |
| `voice_text` | `{"text":"…"}` | 跳过录音/ASR，直接以文字走当前模式的对话链路（底座=设备直连；背夹=要求手机） |

## 3. 底座模式语音（`voice_cloud` 组件）

- 配置存 NVS 命名空间 `vcloud`：`asr_base`（如百炼业务空间域名，不含路径）、`asr_key`、`asr_model`（默认 `qwen3-asr-flash`）、`chat_base`（OpenAI 兼容根，如 `https://api.deepseek.com`）、`chat_key`、`chat_model`（默认 `deepseek-flash`，请求带 `"thinking":{"type":"disabled"}`；百炼根则用 `<base>/compatible-mode/v1`）。日志与任何返回都不得出现密钥。
- ASR：录音 Opus（16 kHz、单声道、60 ms 帧）→ Ogg Opus 封装（OpusHead/OpusTags + 音频页，granule 按 48 kHz 计）→ base64 → `POST <asr_base>/compatible-mode/v1/chat/completions`，`messages=[system 热词上下文, user [{type:input_audio, input_audio:{data:"data:audio/ogg;base64,…"}}]]`，`asr_options:{enable_itn:true, language:"zh"}`。
- 对话：系统提示（背夹助手、回答简短≤80 字、中文）+ 最近 4 轮 + 工具：`open_app{name}`、`set_brightness{percent}`、`set_volume{percent}`、`set_mode{mode}`；最多 2 轮工具调用。
- 状态回调：`recognizing → heard(text) → thinking → reply(text)` 或 `error(msg)`，由首页语音卡片显示。

## 4. 交付与验收原则

- 只写应用分区 `0x20000` 与 SYSTEM 分区（刷前整片/分区备份）；不改分区表、不动 NVS/storage/SD 卡原有应用。
- 每个模块：宿主测试 →（UI）宿主预览截图 → 目标板构建 → 上机。需求、源码、仿真、构建、实机分别标注。

## 5. 语音本地指令（2026-10-16 补充）

实测对话模型（deepseek-flash）会只回“音量已经调到 40% 了”却不调用 `set_volume`。1.0 在两条语音线路上都先做本地指令：
`components/common/system_ui/src/home_intent.c` 纯文本解析（宿主测试 `test_host/test_home_intent.c`），`home_platform.c` 的 `hv_intent_run` 执行。

- 底座·Wi-Fi：`voice_cloud_set_intent()` 回调在识别之后、请求对话模型之前调用；认得就不再请求模型，问答照常进历史。
- 背夹·手机：手机识别后发 `voice_intent`；`handled=true` 时手机直接把 `reply` 用 `assistant_show` 推回，不问模型。
- 认的范围：音量/亮度的目标值（阿拉伯数字、中文数字、“百分之N”）与“大一点/小一点/静音/最大”；切换模式须有切换动作（问句不算）；“打开/启动/进入/玩 + 应用名”且能在应用目录里找到。其余（问句、没给数值、目录里没有的名称）仍交给模型。
- 打开应用沿用“先显示回复、约 1.8 秒后再打开”。

## 6. 设备主动推送（2026-10-16 补充）

- `{"id":"mode-event","ok":true,"type":"mode_event","mode":"clip"|"dock"}`：背夹上切了模式（控制中心、语音指令或手机 `mode_set`）时推送；手机据此刷新模式卡。旧 App 忽略即可。

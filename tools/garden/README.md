# 「我的」花园（my_garden）· 检查、单测、仿真工具

日期：2026-10-16。花园本体在 `application/edge_agent/fatfs_image/system/skills/my_garden/`（随固件烧进只读 SYSTEM 分区）；
本目录是开发与验收用的工具，不进固件镜像。验证结果见 [VALIDATION.md](VALIDATION.md)（全部是**仿真**与宿主测试，未上实机）。

## 花园是什么

固件 1.0 的「我的」：系统首页点「我的」小组件或总览里的「我的 · 小磁」启动的 Lua 应用（独占显示）。四个主要页面：

| 页面 | 内容 |
| --- | --- |
| 我的 · 小磁 | 主题化的小磁场景 + 气泡（问候随时段 / 心情 / 事件变化）、心情 / 能量 / 陪伴三张卡、小家与花园入口、形象与主题一排卡 |
| 小家 | 主题外壳的房子剖面（沿用其三套主题与房间应用），焦点房间实时预览、房间里玩贪吃蛇，小磁一格一格走到焦点房间 |
| 花园 | 2 列 × 3 行地块（两页，最多 11 株），每块一株沙箱里的 Lua 植物：添加、移除、换位置、浇水、热重载 |
| 个性化 / 一句话生成 | 名字与强调色；AI 作品清单与载入失败原因 |

接口约定（与 `docs/backclip-1.0/CONTRACT.md` 第 1.1、1.2 节一致）：

| 项 | 约定 |
| --- | --- |
| 启动 | `cap_lua_run_script_async("<SYSTEM>/skills/my_garden/scripts/main.lua", args_json, 0, "my_garden", "display", true, ...)` |
| args | 固件注入全局 `args`：`{"page":"mine"|"house"|"garden"}`；缺省 mine。直达小家 / 花园时页面栈是「我的」→ 目标页，SET 先回「我的」 |
| 退出 | 「我的」里按 SET：主循环结束（落盘、写快照、释放按键）→ 回系统首页；底部上滑由固件直接停止（数据平时节流落盘） |
| 模块解析 | 固件把脚本目录放在 `package.path` 最前面：`require("core.app")` → `<SYSTEM>/skills/my_garden/scripts/core/app.lua` |
| 数据 | `<DATA>/garden/`（DATA 根取 `storage.get_root_dir()`，拿不到用 `/sdcard`）：`garden.json`、`snapshot.json`、`plants/*.lua`、`themes/*.lua`、`ui_font.ttf` |
| 快照 | 契约 1.2 的字段全部写出；状态变化（≥ 2 s 节流，内容不变不写）与退出时写；`.tmp` → 删旧 → rename |

## 按键与触摸

| 实体键 | 逻辑 | 作用 |
| --- | --- | --- |
| VOLUME_DOWN | prev | 上一个 |
| MODE | ok | 确认；按住 0.6 s（花园自己计时，不用板级 2 s 长按）= 拿起地块 / 植物页“更多” / 小磁上 = 个性化 |
| VOLUME_UP | next | 下一个 |
| SET | back | 返回上一级；拿起地块时 = 放下；在「我的」= 退出 |

底部软键栏四格与四颗键一一对应、文字随页面变；触摸点软键 = 按键，长按 M 格 = 长按 MODE。
触摸：点卡片、地块和房间“第一下聚焦、第二下打开”，长按地块拿起后点目标地块就挪过去，左右滑翻页（tileview），页头左上角返回。

## Lua 特性用在哪

| 特性 | 位置 | 做什么 |
| --- | --- | --- |
| `_ENV` 沙箱 | `core/sandbox.lua` `M.env` / `M.load` | `load(src, 名字, "t", env)`：植物和自定义主题的全局环境是白名单表（math/string/table/utf8/coroutine/只读的 os 时间函数），没有 io、require、load、debug、package、固件模块；每株植物一份环境，互相看不见；拒绝预编译字节码 |
| `pcall` / 协程错误隔离与降级 | `core/sandbox.lua` `M.guard`、`core/garden.lua` `g.call` / `g.wither` | 每次调用植物都放进新协程执行，出错只把这一株画成“枯萎 + 第几行什么错”；主题画小家失败降级为极简；页面回调出错只记 `GD_ERR` |
| `debug.sethook` 指令预算 | `core/sandbox.lua` | 每 1000 条指令计一次数，超预算抛唯一哨兵错误；沙箱里的 `pcall` / `coroutine.resume` 不能吞掉它，植物自己开的协程也挂同一个预算 |
| 协程驱动动画与后台任务 | `core/sched.lua`，用在 `pages/house.lua`（小磁 8 像素一格走到焦点房间）、`pages/garden.lua`（浇水水滴落下）、`core/fonts.lua`（字体分块复制，每帧最多占 6 ms） | 把“分很多帧完成”的事写成顺序代码；`task.sleep(ms)` 让出 |
| 元表 | `core/themes.lua` `palette`（用户配色 → 主题 token → 基础 token 三层 `__index` 继承）、`style`（缺省项由 token 推出）、`readonly`（`__newindex` 报错、`__metatable` 锁）；`core/plantui.lua` 句柄“类”（`__index` 共享方法、锁住元表，原始 LVGL 对象藏在私有弱表）；`pages/house.lua` 反白卡片的颜色覆盖；`core/ui.lua` 弱键表缓存文字 | 继承、只读、封装、按对象缓存 |
| 闭包事件处理 | `core/input.lua` `bind_tap` / `bind_soft`，各页面 | 点按回调捕获地块序号 / 房间序号；只“投递”到主循环执行，不在 LVGL 回调里改界面 |
| 热重载 | `core/garden.lua` `g.scan` / `g.reload` | 每 3 秒看一眼 `<DATA>/garden/plants/`（设备用大小 + 修改时间，仿真没有修改时间就比较内容片段），变了就重新 `load`，换模块、留状态、复活枯萎地块 |
| JSON 持久化 | `core/json.lua`（纯 Lua 编解码，键排序输出）、`core/store.lua`（`.tmp` → 删旧 → rename、`.tmp` 恢复、坏文件另存 `.bad`） | 宿主、仿真、设备三处行为一致；FAT 不能覆盖式 rename |
| 可变参数 / `table.pack` | `core/app.lua` `app.safe`、`core/sandbox.lua` | 完整转发多返回值（含 nil） |

## 工具

| 命令 | 作用 |
| --- | --- |
| `python3 tools/garden/gd.py check` | 宿主 Lua 5.5 语法检查（代码保持 5.4 兼容）、设备字体逐字检查（`NotoSansSC-Regular-sub.ttf`，3680 字；`! ^ ° → · ● … ←` 等按约定也算错）、仅仿真缺字单列、入口 64 KB 检查、SKILL.md frontmatter 按 claw_skill 解析器要求检查 |
| `python3 tools/garden/gd.py unit` | 宿主 Lua 单测（`tests/test_*.lua`）：JSON、沙箱、持久化（FAT 语义的假 storage）、花园模型、小磁 |
| `python3 tools/garden/gd.py plant-check <植物.lua>` | 第三方 / AI 植物检查：静态规则、文件名、体积、设备字形，再用宿主 Lua 在同一份沙箱里试跑（种下、卡片、全屏页、按键、30 秒 tick、浇水、状态可序列化），报告对象用量 |
| `~/.cache/backclip-web/venv/bin/python -I tools/garden/gd.py sim --play keys.json [--touch t.json] [--page garden] [--data fixtures/data_ai] [--out 目录]` | 在 480×800 同源仿真里跑一次 |
| `~/.cache/backclip-web/venv/bin/python -I tools/garden/gd.py scen tools/garden/scenarios <输出目录> [--only 01_]` | 批量跑场景，汇总 `RESULTS.md`（断言、截图、页面构建、性能、状态序列） |
| `python3 tools/garden/scenarios/make.py` | 重新生成场景时间轴（改了布局或流程后） |
| `python3 tools/garden/sheet.py 输出.png 截图...` | 把截图拼成对照图 |

依赖：系统 `python3` 带 fontTools、Pillow（`check`、`plant-check`、`sheet.py`）；`~/.cache/backclip-web/venv` 带 Playwright（仿真，必须 `-I`）；
宿主 Lua `~/.cache/backclip-web/host-lua-5.5.0/lua`；仿真构建按 `ESP_CLAW_LVGL_SIM_DIR` → `~/.cache/backclip-web/esp_claw/build/lua_lvgl_web_sim` →
`~/.cache/backclip-web/build` 的顺序查找（与主题外壳 xj.py 一致）；Chromium 用 Playwright 自带的，找不到时按 xj.py 的办法从 `~/Library/Caches/ms-playwright` 里挑。

### 仿真怎么接上花园

- 花园脚本挂到与设备相同的路径 `/system/skills/my_garden/scripts/`；仿真的 DATA 根是 `/storage`（设备是 `/sdcard`），数据目录 `/storage/garden/`。
- 仿真不注入 `args`：工具把启动参数写成 `sim_args.json`、按键时间轴写成 `autoplay.json`，只挂进仿真（设备 SYSTEM 只读，没有这两个文件）。
- 仿真的 `button` 模块是桩，实体键靠时间轴注入（`{t, key, kind}`，`mark` 截图点会让后续按键顺延 700 ms）；触摸用 Playwright 在画布上真实点按、长按、拖动（墙钟毫秒）。
- 运行中改文件：触摸时间轴里的 `{"do": "write", "path": "/storage/garden/plants/x.lua", "file": "fixtures/…"}`，模拟手机 / 电脑推送新植物（热重载验证）。
- 日志：`GD_STATE`（页面栈、焦点、模式）、`GD_PAGE`（每次建页的对象数与耗时）、`GD_PERF`（每 10 s：主循环平均 / 最差、对象数、Lua 内存）、
  `GD_WITHER` / `GD_PLANT_FAIL` / `GD_RELOAD`、`GD_SNAPSHOT`、`GD_MARK`（截图）、`GD_ERR` / `GD_FATAL`（失败）。

## 场景（`scenarios/`，由 `make.py` 生成）

| 场景 | 内容 |
| --- | --- |
| `01_keys_full_flow` | 按键主线：我的 → 三张卡 → 戳小磁 → 小家实时预览 → 回我的切极简 → 极简样式的小家 → 花园 → 种子库种下 → 长按拿起、移动、SET 放下 → 全部浇水 → 种下故意报错的 AI 植物（只有它枯萎）→ 宿主推送修好的文件，热重载复活 → 顶层 SET 退出 |
| `02_touch_full_flow` | 触摸主线：点小磁、卡片、房间两段式点按与房间里游玩、页头返回、点主题卡、地块点按、软键代替实体键、植物页按钮、长按拿起后点目标、左右滑翻页、页头全部浇水、软键 SET 退出 |
| `03_ai_isolation_theme` | 死循环（指令预算）、越权（沙箱）、语法错误、正常 AI 植物、更多面板、一句话生成页、主题外壳 v1 接口的 AI 主题（复古电台） |
| `04_args_house_back` | `args.page=house` 直达，SET 逐级返回再退出 |
| `05_saved_data` | 读回已保存的 garden.json（名字、主题、配色、地块顺序与状态、陪伴天数） |
| `06_corrupt_data` | garden.json 损坏：另存 .bad、默认值重建 |
| `07_personalize` | 换名字、换强调色，页头 / 标签 / 快照跟着变 |
| `08_rooms_play_water` | 房间里玩贪吃蛇到结束；喝水提醒房间喝两杯，小磁与快照联动 |
| `09_full_garden` | 满园 11 株（两页、没有“种植物”格）：对象数在预算内；复杂像素图在卡片上按预算降级为文字而不枯萎 |
| `10_no_font_fallback` | 不复制字体、不建 tileview：分页降级为普通容器，按键 / 点页码翻页照常 |
| `11_saved_ai_theme` | 上次选的 AI 主题重启后直接恢复（先扫描自定义主题再套用） |

## 固件侧注意

- `SKILL.md` 的 `execution.visible = false`：入口由系统首页的「我的」小组件与总览（`home_catalog.c` 里的 `garden`）负责，不在旧轮播重复出现。
- 只有可左右滑的分页（花园、种子库）用 tileview；tileview 必须挂从 DATA 加载的中文字体，所以首次运行会在后台把系统字体复制到
  `<DATA>/garden/ui_font.ttf`（约 1.1 MB，主题外壳已经复制过的 `xiaojia/ui_font.ttf` 直接复用）。复制完成前翻页用按键 / 点页码。

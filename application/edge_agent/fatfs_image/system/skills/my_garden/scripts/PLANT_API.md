# 「我的」花园 · 植物接口 v1（人和 AI 共用的契约）

一株“植物”就是一个小 Lua 文件。它长在花园的一块地里（2 列 × 3 行一页），点开是它自己的全屏页。
符合本接口的文件放进 `<DATA>/garden/plants/<id>.lua`（样机 DATA 根为 `/sdcard`），几秒内出现在种子库里，带 AI 标记。
内置的 8 颗种子（`scripts/plants/*.lua`）也完全按本接口写成、走同一个沙箱，可以当范例。

## 1. 文件与命名

- 文件名就是 id：小写字母开头，只用小写字母、数字、下划线，例如 `standup.lua`。
- 文件最后 `return` 一个表；源码不超过 16 KB；只能是文本（预编译字节码会被拒绝）。
- 界面文字必须是设备字体里有的字（3680 字子集）。**不要用** `!` `^` `°` `→` `←` `◀` `▶` `·` `●` `…` `×` 和表情符号；
  需要感叹号用全角 `！`，需要箭头写“上一个 / 下一个”，引号用 `「」`。

## 2. 模块形状

```lua
local P = {
  name = "起来走走",                 -- 必填：显示名（地块标签、种子库、页头）
  desc = "坐久了提醒你起来活动",       -- 种子库里的一行说明
  icon = { "...kk...", "..kkkk..", ".k.kk.k.", "...kk...", "..k..k..", ".k....k." }, -- 可选：≤ 12×12 像素图标，"." 透明
  pal  = { k = "accent" },          -- 图标调色板：字符 → "#RRGGBB" 或主题颜色名
  care = true,                      -- 可选：false = 不需要浇水（例如计数器）
}
function P.init(s, ctx) end                        -- 种下时调用一次：在 s 里放初始状态
function P.tick(s, dt, ctx) end                    -- 可选：约每 0.5 秒一次（离开花园也在走），只改数据、不画图
function P.summary(s, ctx) return "12", "天" end    -- 可选：一眼概述；没有 card 时用它画默认卡片
function P.card(ui, s, ctx) return { update = function(now) end } end            -- 地块卡片
function P.open(ui, s, ctx) return { update = ..., key = ..., labels = ... } end   -- 全屏页
function P.on_water(s, ctx) end                    -- 可选：被浇水时（每天最多一次）
return P
```

至少要有 `card`、`open`、`summary` 其中一个。缺 `card` 时核心用 `summary` 画卡片；缺 `open` 时全屏页也用 `summary`。

### 状态 `s`

- 每块地一份，**只放数字、字符串、布尔和表**（核心会把它写进 `garden.json`，函数和循环引用会丢）。建议小于 8 KB。
- 同一种植物可以种好几株，每株的 `s` 互不相干；热重载换代码时 `s` 保留。
- 改完 `s` 不用手动保存：核心约 1.5 秒内落盘；想立刻落盘调用 `ctx.save()`。

### 视图（card / open 的返回值）

| 字段 | 说明 |
| --- | --- |
| `update(now)` | 刷新。卡片：焦点地块 1 秒一次，同页其他地块 5 秒一次，别的页不刷新；全屏页：默认 1 秒一次 |
| `rate` | 可选（全屏页）：刷新间隔毫秒，最小 200 |
| `key(k)` | 全屏页的按键：`k` 是 `"prev"` / `"ok"` / `"next"`（只有短按）。处理了返回 `true` |
| `labels()` | 全屏页软键文字：`{ prev = "减一", ok = "加一", next = "归零" }`，**每个不超过 4 个字**，空串 = 这个键没用 |

SET（返回）和长按 M（浇水 / 重新加载 / 移除）归核心，植物收不到。全屏页里植物没用 M 时，M = 浇水（口渴时）或“更多”。

## 3. 画笔 `ui`

坐标都相对这株植物自己的区域：卡片约 186×124，全屏页约 414×540（`ui.W`、`ui.H` 是准确值）；超出区域的部分会被裁掉。
颜色写 `"#RRGGBB"`，或者主题颜色名：`bg` `surface` `surfaceAlt` `line` `text` `textMuted` `accent` `onAccent` `danger` `success`
（用颜色名，换主题时自动变色）。默认字号 20，只有这一种字号；大数字用 `ui.digits`。

| 函数 | 返回 | 说明 |
| --- | --- | --- |
| `ui.box(x, y, w, h, color, {radius, border, border_color, opa})` | 句柄 | 色块；`color` 为 nil 时透明 |
| `ui.text(x, y, str, color, {w, h})` | 句柄 | 文字；给了 `w` 会自动折行（多出 `h` 的部分被裁掉） |
| `ui.digits(x, y, slots, px, color, style)` | 句柄 | 5×7 点阵大数字，支持 `0-9 : / % + - . 空格`；每位约 35 个对象，卡片里别用 |
| `ui.sprite(x, y, px, rows, pal)` | 句柄 | 像素图：`rows` 是 ≤ 32 行、每行 ≤ 32 个字符的字符串数组 |
| `ui.bar(x, y, w, h, ratio, color, bg)` | 句柄 | 进度条（2 个对象） |
| `ui.fit(str, w)` / `ui.wrap(str, w, lines)` / `ui.text_w(str)` | 字符串 / 数字 | 按宽度截断成一行（以 ... 结尾）/ 折成几行 / 估算宽度 |
| `ui.left()` / `ui.used()` | 数字 | 剩余 / 已用对象预算（复杂图形可以据此降级成文字） |

句柄方法：`h:text(s)`、`h:color(c)`（文字色）、`h:fill(c)`（底色）、`h:border(c, w)`、`h:pos(x, y)`、`h:size(w, h)`、
`h:show(bool)`、`h:set(v)`（digits 设文字、bar 设 0～1）、`h:draw(rows)`（sprite 换图）、`h:move(x, y)`、`h:on_tap(fn)`（触摸点按）。
句柄是锁住的：拿不到底层 LVGL 对象，也改不了方法。

**对象预算**：卡片 32 个、全屏页 240 个（一个 box / text 算 1 个，sprite 按行合并后的色块数，digits 每位 35 个）。超了就报错枯萎。
`update` 里只改文字、颜色、位置，不要每次新建对象。

## 4. `ctx`

| 字段 | 说明 |
| --- | --- |
| `ctx.now()` | 开机以来的毫秒数（做动画、计时） |
| `ctx.time()` | Unix 秒；设备没对时返回 0 |
| `ctx.clock_ok()` | 设备时间是否可信（没联网对时可能停在 1970 年，这时别按日期记账） |
| `ctx.date(fmt)` | 当前时间按 `strftime` 格式化，默认 `"%Y-%m-%d"` |
| `ctx.toast(msg)` | 屏幕底部提示条（2 秒） |
| `ctx.save()` | 立刻落盘（一般不用） |
| `ctx.log(msg)` | 写一行日志（`GD_PLANT <id> msg`） |
| `ctx.theme` | `{ id, name, digits }`（只读信息） |
| `ctx.C` | 当前主题颜色表（只读） |
| `ctx.pet.name()` / `mood()` / `mood_text()` / `energy()` / `days()` | 小磁的名字、心情 0～100、心情文字、能量 0～100、陪伴天数 |
| `ctx.pet.today()` | `{ date, cups, focus, cares, plays }` 今天的计数（只读副本） |
| `ctx.pet.cheer(kind, amount)` | 让小磁知道发生了什么：`"water"` 喝了一杯水、`"unwater"` 撤销一杯、`"focus"` 完成一次专注、`"play"` 玩了一会儿、`"mood"` 记了心情（`amount` 1～5）。每天有上限，刷不了 |

沙箱里能用的全局：`math` `string` `table` `utf8` `coroutine`、`os.time` `os.date` `os.clock` `os.difftime`、
`pairs` `ipairs` `next` `select` `type` `tostring` `tonumber` `pcall` `xpcall` `error` `assert` `setmetatable`（只对表）
`getmetatable`（只对表）`rawget` `rawset` `rawequal` `rawlen`。**没有** `io`、`require`、`load`、`dofile`、`debug`、`package`、
`collectgarbage`、`print`，也没有任何固件模块（storage、lvgl、board_manager……）。每株植物有自己的一份全局环境，互相看不见。

## 5. 运行规则

- **指令预算**：每次回调都在单独的协程里执行并计指令数（装载约 60 万条、画卡片 30 万、update 6 万、按键 8 万）。
  死循环会被打断；`pcall` 吞不掉“预算用尽”；自己开的协程也计在同一份预算里。
- **内存**：单次回调新增超过约 1 MB 判为失败。
- **出错 = 枯萎**：任何回调出错，这一株显示“枯萎”和错误摘要（例如 `第 12 行：attempt to index a nil value`），别的地块照常；
  点开能看到完整原因和文件位置。修好文件后会自动热重载并复活，状态保留；也可以在植物页长按 M 选“重新加载”。
- **热重载**：花园、种子库、植物页、一句话生成页每 3 秒看一眼 `<DATA>/garden/plants/`，文件新增、修改、删除都会反映出来。
- 植物看不到别的植物的数据，也不能改主题颜色。

## 6. 检查

```sh
python3 tools/garden/gd.py plant-check 我的植物.lua   # 静态规则 + 设备字形 + 宿主沙箱试跑（种下、卡片、全屏页、按键、30 秒 tick、浇水）
```

## 7. 完整示例

```lua
local P = { name = "起来走走", desc = "坐久了提醒你起来活动", icon = { "..kk..", ".kkkk.", "..kk..", ".k..k." }, pal = { k = "accent" } }
local SIT = 50 * 60
function P.init(s) s.sat = 0; s.stands = 0 end
function P.tick(s, dt) s.sat = math.min(SIT, s.sat + dt / 1000) end
function P.summary(s) return s.sat >= SIT and "该起来了" or (math.floor((SIT - s.sat) / 60) .. " 分钟后提醒"), "今天站起 " .. s.stands .. " 次" end
function P.card(ui, s)
  local t, sub, bar = ui.text(0, 2, "", "text"), ui.text(0, 34, "", "textMuted"), ui.bar(0, ui.H - 14, ui.W, 10, 0, "accent")
  local function paint() local v, l = P.summary(s); t:text(v); sub:text(l); bar:set(s.sat / SIT) end
  paint()
  return { update = paint }
end
function P.open(ui, s, ctx)
  local big = ui.text(0, 20, "", "text", { w = ui.W })
  local self = {}
  function self.update() big:text((P.summary(s))) end
  function self.key(k) if k == "ok" then s.sat = 0; s.stands = s.stands + 1; ctx.pet.cheer("play"); self.update(); return true end end
  function self.labels() return { ok = "站起来了" } end
  self.update()
  return self
end
return P
```

---

# 自定义主题（`<DATA>/garden/themes/<id>.lua`）

沿用主题外壳的**主题接口 v1**（`嵌入式/theme-shell/custom/主题接口.md`）：用 `xj.py theme-gen` / `theme-test` 生成、检查过的主题文件，
直接放进这个目录就能用，出现在「我的」的主题卡一排（带 AI 标记）。主题只作用于「我的」、小家和花园的内容区；状态栏和软键栏是系统外框。

- 必填：`id`（与文件名一致）、`name`、`tokens`（10 个颜色，正文对底色对比度 ≥ 4.5）；
- 小家页调用 `build(ctx, f)`，只用返回的 `view.hub`（房子剖面）。花园会传 `f.hub_only = true`，新主题可以据此只画中枢；
  老主题画出来的状态栏、首页、控制中心被放进屏幕外的容器，不显示；
- v2 可选扩展（花园用）：`tag`（主题卡小字）、`style = {border, shadow, radius, ink, chip, chip_text, focus, seg_radius}`（卡片风格）、
  `say`（小磁的主题台词）、`mascot(ctx, parent, x, y, px) -> {box, w, h, set(mood, now)}`（形象，`mood` 为
  `idle/happy/sleepy/sad/talk/listen/think`）、`scene(ctx, parent, w, h) -> {ground}`（「我的」场景底图，`ground` 是形象脚踩的 y）。
  没写 `mascot` 时用像素小磁，没写 `scene` 时画一条地面。
- 主题同样在沙箱里装载与绘制（没有 io / require 等），出错时小家页降级为极简样式，坏主题不进主题卡并在「一句话生成」页列出原因。

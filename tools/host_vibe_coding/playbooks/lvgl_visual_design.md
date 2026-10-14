---
{
  "name": "lvgl-visual-design",
  "description": "LVGL 界面视觉质量规范：色板、层级、布局、交互反馈和状态完整性",
  "match": { "ui_stack": ["lvgl"] },
  "priority": 5
}
---

生成 LVGL UI 时，目标不是“摆几个控件能跑”，而是接近设备内置应用的完成度。
参考设备端已有的 `snake_game`、`dino`、`game_2048` 风格：清晰舞台、顶栏 HUD、
卡片化状态区、圆角边框、触摸反馈、运行/结束/退出状态都完整。即使用户只说
“做一个界面/小游戏”，也要主动补齐视觉层次和状态反馈。

如果用户反馈“界面会跟着滑动”“弹窗一直存在”“透明按钮还能点到”，优先按本文件的
滚动/浮层规则修复，而不是只调颜色或重写玩法。

## 视觉系统

先定义一组命名色板，不要在代码各处散落裸颜色：

```lua
local C_BG        = "#000000"
local C_PANEL     = "#080C08"
local C_SUNKEN    = "#101820"
local C_ACCENT    = "#F0C428"
local C_ACCENT_HI = "#FFE45B"
local C_DIM       = "#384038"
local C_TEXT      = "#FAFBF5"
local C_TEXT_DIM  = "#788078"
local C_DANGER    = "#F65E3B"
local C_OK        = "#3FA34D"
```

可以按主题换色，但必须保持：背景、面板、内容、强调色、正文、次要文字都有明确分工。
不要做全屏单色块、默认灰按钮、无边框控件堆叠。

根据任务选择主题：

- 工具/仪表/控制台：优先用深色科技主题，接近 `snake_game`，背景近黑、金色或绿色强调。
- 棋盘/数字/休闲游戏：可用 `game_2048` 的暖色主题，浅米背景、深文字、彩色 tile、圆角棋盘和阴影。
- 跑酷/像素小游戏：可用 `dino` 的简洁像素主题，白底或浅底、黑/灰角色、地面线、像素块障碍和 HUD。

暖色棋盘主题示例：

```lua
local C_BG        = "#F8F5EF"
local C_STAGE     = "#F8F5EF"
local C_PANEL     = "#9D8F84"
local C_GRID      = "#BBAFA5"
local C_EMPTY     = "#CDC4BB"
local C_SHADOW    = "#DED6CD"
local C_TEXT      = "#FFFAF4"
local C_TEXT_DARK = "#5B544D"
local C_GOLD      = "#EDC22E"
local C_DANGER    = "#F65E3B"
```

## 布局层级

1. 每个上屏 UI 至少包含一个清晰的主容器结构：顶栏 / 内容区 / 操作区。
2. 顶栏用于标题、状态或计分；高度按屏幕算，例如 `TOP_H = 56`，不要写死只适合 240p 的数值。
3. 内容区用卡片、棋盘、列表或仪表承载主体。卡片必须有 `radius`、`bg_color`、必要时 `border_color`。
4. 操作按钮要集中成可扫视的区域，不要散落在屏幕边角。
5. 所有关键尺寸从 `scr:get_size()` 或 `board_manager.get_display_lcd_params()` 得到的宽高推导。
   横屏下优先左右分栏；竖屏下优先上下分区。

优先使用“居中舞台”布局，尤其是游戏和可视化界面：

```lua
local stage = math.min(screen_w, screen_h)
local sx = math.floor((screen_w - stage) / 2)
local sy = math.floor((screen_h - stage) / 2)
local margin = math.max(10, math.floor(stage * 0.03))
local header_h = math.max(48, math.floor(stage * 0.13))
local content_w = stage - margin * 2
```

舞台内再分：标题块 / 状态卡片 / 主内容面板。参考 `game_2048`：标题块在左，分数/最佳分在右，
中间空间足够时放退出或重置按钮；主棋盘/主面板居中，带 2-4px 阴影层。参考 `dino`：
跑酷类界面要有地面线、远景小装饰、角色阴影、右上角 HUD，不要让角色悬在空白背景上。

## 控件风格

- `button`：设置 `bg_color`、`text_color`、`radius`、`border_color`、`border_width`。
- `container`：用于面板/卡片/棋盘格；设置 `bg_opa=255`、`radius`、`pad=0`，不需要滚动时关闭滚动条。
- `label`：主标题用高对比色；提示文字用 `C_TEXT_DIM`；状态/得分用 `C_ACCENT`。
  普通中文/英文标签直接使用 `lvgl.init({ font_size = ... })` 提供的默认字体。
  不要为了改字号手动 `lvgl.font_load(storage.join_path(storage.get_root_dir(), "fonts/NotoSansSC-Regular-sub.ttf"), ...)`；
  DATA 根目录不一定有这个字体，加载失败会让脚本在创建 UI 阶段直接退出。需要字体文件时必须 `pcall`
  并有失败降级，否则不要设置 `font` 样式。
- 游戏或动态元素用小型 `container` 作为对象，靠 `set_pos()` 移动，不要每帧重建整屏。
- 交互按钮要有按下反馈：点击时临时改成高亮色，主循环里若干 tick 后恢复。
- 计分/状态卡片采用两行文字：上方小号标签，例如 `SCORE` / `BEST` / `状态`；下方大号数值。
- 棋盘或网格必须有底板、间距、空格背景、当前项高亮；不要只画孤立方块。
- 像素风角色可由多个小 `container` 或短线/矩形组成，保持块状轮廓；同时添加阴影、障碍和地面基线。
  组合角色的父容器只是分组和移动锚点，必须是完全不可见：`bg_opa=0`、`border_width=0`、
  `radius=0`、`pad=0`，并关闭滚动。不要让父容器留下可见方框；可见身体只由帽子、脸、
  身体、腿等子对象构成。
- 如果屏幕旁边留白明显，可放低对比装饰纹理，例如浅色数字、短线或几何块，但不能抢主内容。

全屏游戏和工具面板默认都不是滚动页面。创建根屏、舞台、HUD、overlay 后立即禁用滚动：

```lua
local function no_scroll(obj)
    obj:set_scroll({ dir = "none", scrollbar = "off" })
end

no_scroll(scr)
no_scroll(stage)
no_scroll(hud)
```

只有明确做列表、菜单、长文本时才允许开启滚动；游戏滑动手势绝不能拖动 UI 容器本身。

示例：

```lua
local function make_key(parent, text, x, y, w, h)
    local key = lvgl.button(parent, {
        text = text,
        x = x,
        y = y,
        w = w,
        h = h,
        bg_color = C_PANEL,
        text_color = C_TEXT,
        radius = 14,
        border_color = C_DIM,
        border_width = 3,
    })
    return key
end
```

## 状态完整性

1. 不只生成初始画面，还要生成运行中、成功/失败/空状态、退出或重开状态。
2. 游戏结束或操作完成不要直接退出脚本；显示 overlay/card 提示，让用户能看到结果。
3. DEBUG 版保留关键 `dbg()`：初始化尺寸、触摸注册、点击事件、状态变化、异常分支。
4. Release 版去掉调试打印，但保留用户可见状态和必要错误处理。

结果浮层要像产品界面而不是日志文本：居中卡片、半透明或实色面板、强调色边框、标题、说明、主要按钮。
例如游戏结束显示 `GAME OVER`、得分、`点击重开`；任务完成显示成功标题和下一步操作。不要只 `print()`。

隐藏浮层不要只改透明度。透明对象仍占布局和命中区域，会出现“游戏开始后失败窗口还在”或
“看不见的按钮仍可点”的问题。推荐模式：

```lua
local overlay_visible = false
local overlay_x = math.floor((screen_w - overlay_w) / 2)
local overlay_y = math.floor((screen_h - overlay_h) / 2)

local function set_overlay_visible(visible)
    overlay_visible = visible
    if visible then
        overlay:set_pos(overlay_x, overlay_y)
        overlay:set_style({ bg_opa = 235, border_width = 2 })
    else
        overlay:set_pos(-10000, -10000)
        overlay:set_style({ bg_opa = 0, border_width = 0 })
    end
end
```

如果用手动触摸命中检测，必须先判断 `overlay_visible`；如果用 LVGL 按钮回调，隐藏时把整个
overlay 移到屏幕外，重开时再移回。

## 游戏对象与碰撞

动态游戏对象要分清三层：

- **视觉对象**：实际显示的 LVGL 对象，可以由多个子对象组成。
- **状态数据**：`x/y/vx/vy/w/h` 等，主循环唯一可信来源。
- **碰撞盒**：`hit_x/hit_y/hit_w/hit_h`，相对状态坐标，用于命中判定。

不要用装饰子对象、阴影、透明父容器或文字标签参与碰撞。不要从 `obj:get_pos()` 读取位置做碰撞。
创建对象时生成一次尺寸，并把同一组 `w/h` 同时用于 LVGL 对象和状态表。禁止在创建对象和保存状态时
各随机一次宽高，否则图形和碰撞盒会错位。

推荐碰撞辅助：

```lua
local function hitbox(item)
    return item.x + (item.hit_x or 0),
           item.y + (item.hit_y or 0),
           item.hit_w or item.w,
           item.hit_h or item.h
end

local function overlap(a, b)
    local ax, ay, aw, ah = hitbox(a)
    local bx, by, bw, bh = hitbox(b)
    return ax < bx + bw and ax + aw > bx and ay < by + bh and ay + ah > by
end
```

平台跳跃类角色通常要用比视觉外框更小的 hitbox，避免透明边缘、帽檐、阴影导致误碰。
例如 48x52 的角色可以用 `{ hit_x=6, hit_y=4, hit_w=36, hit_h=46 }`。

## 交互反馈

- `obj:on("clicked", fn)` 回调里只改状态表或设置简短标志；复杂更新放主循环。
- 点击按钮后应该有视觉反馈，例如 `set_style({ bg_color = C_DIM, border_color = C_ACCENT_HI })`，
  再在主循环里恢复。
- 如果有触摸输入，优先注册给 LVGL：`lvgl.indev_register("touch", touch_handle)`。
  不要一边用 LVGL 事件，一边在主循环里轮询 `lcd_touch` 处理同一套触摸。
- 水果忍者、画板、拖拽轨迹、滑动切割等需要连续触点坐标的任务是例外：不要注册 LVGL touch indev，
  获取句柄后、进入主循环前只调用一次 `lcd_touch.sync(handle)` 初始化状态；主循环里只用
  `lcd_touch.poll(handle)` 采样。不要在每帧 `sync()` 后立刻 `poll()`，否则真实设备会丢失
  边沿/增量状态，出现 Web 预览可滑动但设备滑动无效的问题。同时给根屏和所有全屏容器
  `set_scroll({ dir = "none", scrollbar = "off" })`，避免滑动手势滚动画面。

## 禁止项

1. 不要输出只有几个默认按钮和白底/灰底的 demo UI。
2. 不要把 UI 文案写在解释文字里，必须真实显示在 LVGL 对象上。
3. 不要写固定 `320x240` 布局。必须基于当前屏幕宽高计算。
4. 不要让脚本自然走到文件末尾。LVGL UI 必须 `lvgl.run()` 或 `while true do ... lvgl.process_events(...) end`。
5. 不要生成没有主题的“控件列表”。必须能一眼看出这是完成态应用：有标题、主体、状态、反馈、退出/重置入口。
6. 不要把透明度当成隐藏。需要隐藏的 overlay、按钮、提示层必须移出屏幕或删除。
7. 不要同时让 LVGL 和 `lcd_touch.poll` 消费同一个滑动手势。

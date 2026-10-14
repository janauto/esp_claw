---
{
  "name": "game-lvgl",
  "description": "游戏/动画类脚本用 lvgl 对象 + 事件回调的实现规范（跳跃闯关、贪吃蛇、打砖块等）",
  "match": { "task_kind": ["game"], "ui_stack": ["lvgl"] },
  "priority": 10
}
---

游戏用 **lvgl 对象**表达画面，不要用 display 逐帧自绘。每个游戏元素是一个 lvgl 对象，
移动靠 `set_pos` 增量更新。普通点击类输入靠 `on("clicked", ...)` 事件回调；
水果忍者、画线、拖拽轨迹、滑动切割这类需要连续坐标的游戏，必须改用 `lcd_touch.poll(handle)`
在主循环里采样，且不要再把同一个触摸句柄注册给 LVGL。
整个脚本是**一个 Lua job 内的单线程主循环**，不要用协程调度帧。

## 骨架

```lua
local lvgl = require("lvgl")
local board_manager = require("board_manager")

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

-- 触摸：从 board_manager 取 handle，再注册给 lvgl 作为输入设备
local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")
if touch_handle then
    lvgl.indev_register("touch", touch_handle)
end

-- 屏幕尺寸从 board_manager 取，六个返回值，不要硬编码 320/240
local panel_handle, io_handle, screen_w, screen_h, panel_if =
    board_manager.get_display_lcd_params("display_lcd")
screen_w, screen_h = screen_w or 320, screen_h or 240

local scr = lvgl.create_screen()
scr:set_style({ bg_color = 0x87CEEB, bg_opa = 255 })
scr:set_scroll({ dir = "none", scrollbar = "off" })
scr:load()

-- 全部游戏状态放在 Lua 表里，这是唯一权威来源
local state = { score = 0, running = false, items = {} }

local score_label = lvgl.label(scr, { text = "分数: 0", align = "top_mid", y = 15 })

-- 输入：事件回调，不要在主循环里轮询触摸
scr:on("clicked", function()
    if not state.running then state.running = true else jump() end
end)

local function set_pos_int(obj, x, y)
    obj:set_pos(math.floor(x), math.floor(y))          -- 坐标必须取整
end

local function make_invisible_group(parent, x, y, w, h)
    local group = lvgl.container(parent, {
        x = x, y = y, w = w, h = h,
        bg_opa = 0,
        border_width = 0,
        radius = 0,
        pad = 0,
    })
    group:set_scroll({ dir = "none", scrollbar = "off" })
    return group
end

while true do
    if state.running then
        -- 1. 更新状态（改 Lua 表里的数值）
        -- 2. 把状态写进对象位置
        for i = #state.items, 1, -1 do
            local it = state.items[i]
            it.x = it.x - speed
            set_pos_int(it.obj, it.x, it.y)            -- y 用内存里的值
            if it.x < -it.width then
                it.obj:delete()
                table.remove(state.items, i)
            end
        end
        -- 3. 碰撞/计分判定，全部基于 Lua 表里的坐标
    end
    lvgl.process_events(16)                             -- 帧节拍 + 让出 CPU，必须有
end

lvgl.indev_unregister("touch")
lvgl.deinit()
```

## 必守规则

1. **坐标和尺寸必须是整数**。`set_pos` / `w` / `h` / `radius` 传浮点会抛 Lua 错误，不会自动取整。
   物理运算（重力、速度）用浮点算，写进 `set_pos` 前统一 `math.floor`。
2. **每个对象的坐标以 Lua 表里保存的值为权威，不要用 `get_pos` 读回来再算**。
   读回值在移动过程中可能不是你刚写进去的值，拿它做增量会让对象瞬间飞出屏幕。
   创建对象时就把 `x` / `y` / `width` / `height` 存进状态表，之后只写不读。
   随机尺寸只能生成一次并同时用于 LVGL 对象和状态表，禁止写成 `local pw = ...` 创建对象后，
   又在 `state` 里重新 `w = 50 + math.random(...)`。这会让碰撞盒和真实图形不一致。
3. **lvgl 颜色可以用 `"#RRGGBB"` 字符串或 `0xRRGGBB` 数字**。推荐先定义命名色板，
   例如 `local C_PANEL = "#080C08"`，再在 `set_style` / 构造 opts 里复用。
   不要在代码各处散落裸颜色；也不要把 display 模块的绘图 API 和 lvgl 混用。
4. **`get_display_lcd_params` 是多返回值**，必须按位接收，它不是 table，不能 `.width` 访问。
5. **主循环必须调 `lvgl.process_events(ms)`**。它既刷新画面也让出 CPU；
   没有它画面不动且会饿死其他任务。不需要额外 `delay.ms`。
6. **点击按钮/点按跳跃用 `on("clicked", fn)` 事件回调**，不要在主循环里自己采样触摸。
   回调里只改状态表，不要在回调里跑重逻辑。
   **连续滑动/切水果/画轨迹例外**：这类任务不要调用 `lvgl.indev_register("touch", handle)`，
   改为手动采样：进入主循环前 `lcd_touch.sync(handle)` 一次；主循环里只调用
   `lcd_touch.poll(handle)`。绝不能在同一帧里 `sync()` 后立刻 `poll()`，否则真实设备上
   `just_pressed` / `just_released` / `dx` / `dy` 会被 `sync()` 刷掉，Web 预览可能正常但设备滑动失效。

   连续滑动骨架：

   ```lua
   local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")
   assert(touch_handle, "no lcd_touch handle")
   lcd_touch.sync(touch_handle) -- 只在主循环前一次

   local last_x, last_y = nil, nil
   while true do
       local touch = lcd_touch.poll(touch_handle)
       if touch.pressed and touch.x and touch.y then
           if last_x and last_y then
               -- 用 last_x,last_y -> touch.x,touch.y 做滑动轨迹/切割碰撞
           end
           last_x, last_y = touch.x, touch.y
       else
           last_x, last_y = nil, nil
       end
       lvgl.process_events(16)
   end
   ```
7. **对象删除后不能再引用**。移出屏幕的元素先 `obj:delete()` 再从表里 `table.remove`，
   倒序遍历（`for i = #t, 1, -1`）才能边删边遍历。
8. **子对象坐标相对父对象**。给玩家加眼睛、嘴巴时，父对象是玩家本体，
   坐标从 0 开始算，父对象移动时子对象自动跟随，不用单独更新。
9. **label 支持中文**，分数、"游戏结束" 直接写中文即可，这是相对 display 的优势。
10. 布局先算一条基准线（如地面顶部 `baseline_y`），所有元素按基准线定位，
    不要散落硬编码 y 值，否则换分辨率就错位。
11. 游戏结束不要退出脚本，把状态切成 `game_over` 并显示提示 label，
    点击后重置状态表并复位对象位置，让玩家能重开。

12. **脚本绝不能从主循环末尾自然走完**。走完 = 屏幕闪一下就回主界面，判定为失败。
    完整脚本的最后一段必须是 `while true do ... lvgl.process_events(16) end`；
    上位机会让它一直跑到超时，超时才是这类任务的成功状态。

13. **全屏游戏必须禁用滚动**。创建 `scr`、舞台容器、HUD 容器、overlay 面板后都调用：
    `obj:set_scroll({ dir = "none", scrollbar = "off" })`。不要创建比父对象更大的可滚动
    容器；滑动手势只能影响游戏状态，不能让界面整体移动。

14. **隐藏 overlay 不能只把透明度设为 0**。透明对象和透明按钮仍可能占位或被点击。
    推荐两种方式：隐藏时 `panel:set_pos(-10000, -10000)` 并设置 `state.overlay_visible=false`；
    显示时移回居中位置。或者在重开时 `panel:delete()` 后重建。命中检测和按钮逻辑必须先检查
    `state.overlay_visible`。

15. **样式表只能放确定的颜色值**。`bg_color`、`text_color`、`border_color` 不允许传 `nil`、
    table、布尔值或变量名拼错的值；统一使用命名色板里的 `"#RRGGBB"` 字符串或 `0xRRGGBB` 数字。

    普通标签不要手动加载默认中文字体文件。字号通过 `lvgl.init({ font_size = 22 })` 控制即可。
    禁止生成 `lvgl.font_load(storage.join_path(storage.get_root_dir(), "fonts/NotoSansSC-Regular-sub.ttf"), ...)`
    这类代码：DATA 根目录的 `fonts/` 可能不存在，设备会报 `lvgl failed to load font` 并退出。
    只有用户明确提供字体文件路径时才可 `font_load`，并且必须用 `pcall` 包裹，加载失败时继续使用默认字体。

16. **组合角色/精灵的父容器必须是真透明分组**。例如马里奥、飞机、怪物这类由多个子矩形拼成的角色，
    父容器只负责整体移动和承载子对象，必须设置：
    `bg_opa = 0, border_width = 0, radius = 0, pad = 0`，并关闭滚动。
    不要只写 `bg_opa = 0`，否则某些主题/默认样式仍可能留下边框、内边距或可见方框。
    父容器不要参与视觉绘制；可见外形只由子对象组成。

17. **碰撞盒要显式建模，不要直接拿装饰外框当碰撞**。每个可碰撞对象状态表必须包含
    `x, y, w, h`，必要时再加 `hit_x, hit_y, hit_w, hit_h` 做更贴合的命中盒。
    玩家这类组合角色通常用略小于视觉外框的 hitbox，例如身体 48x52 时可用
    `hit_x = 6, hit_y = 4, hit_w = 36, hit_h = 46`，减少帽檐/透明边缘误判。
    所有碰撞检测都基于状态表里的 hitbox，不基于 `obj:get_pos()` 或控件默认尺寸。

18. **创建动态对象时返回完整状态表**。推荐把创建函数写成：

    ```lua
    local function spawn_pipe(x)
        local w = 50 + math.random(0, 15)
        local h = 55 + math.random(0, 70)
        local y = GROUND_Y - h
        local obj = lvgl.container(scr, { x = x, y = y, w = w, h = h, bg_color = C_PIPE, bg_opa = 255 })
        obj:set_scroll({ dir = "none", scrollbar = "off" })
        return { obj = obj, x = x, y = y, w = w, h = h, hit_x = 0, hit_y = 0, hit_w = w, hit_h = h }
    end
    ```

    不要让 `create_pipe()` 只返回对象，再在外面猜测或重新计算宽高。

19. **平台类游戏按钮必须处理按下和释放状态**。左右移动按钮用 `pressed` 设置
    `state.move_left/right = true`，用 `released` 清零；同时在每帧如果 `state.game_over`
    或按钮隐藏，强制清零移动状态。跳跃按钮只设置一次 `state.jump_triggered = true`，
    主循环消费后立即清零，避免长按每帧连续起跳。

## 完整性要求

只输出**能直接运行的完整脚本**：从 `require` 到主循环一次写完。
不要先给示意片段再给完整版，不要用 `-- ...` 或"其余同上"省略任何一段——
上位机会把你的代码原样上传执行，缺一段就是运行时报错。
如果预算紧张，就减少装饰性元素（云朵、眼睛、渐变），不要减少主循环和状态逻辑。

## 无输入设备时

如果规划里 `input_method` 是 `none`，做成**自动演示**：用固定策略驱动，
让画面自己动起来，但同样保持主循环常驻，不要让脚本自己退出。
不要生成一个等触摸但永远等不到的死循环。

---
{
  "name": "game-lvgl",
  "description": "游戏/动画类脚本用 lvgl 对象 + 事件回调的实现规范（跳跃闯关、贪吃蛇、打砖块等）",
  "match": { "task_kind": ["game"], "ui_stack": ["lvgl"] },
  "priority": 10
}
---

游戏用 **lvgl 对象**表达画面，不要用 display 逐帧自绘。每个游戏元素是一个 lvgl 对象，
移动靠 `set_pos` 增量更新，输入靠 `on("clicked", ...)` 事件回调。
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
3. **lvgl 的颜色是数字 `0xRRGGBB`**（如 `bg_color = 0x4CAF50`）。
   这跟 display 模块相反——display 用字符串，lvgl 用数字，不要混。
4. **`get_display_lcd_params` 是多返回值**，必须按位接收，它不是 table，不能 `.width` 访问。
5. **主循环必须调 `lvgl.process_events(ms)`**。它既刷新画面也让出 CPU；
   没有它画面不动且会饿死其他任务。不需要额外 `delay.ms`。
6. **输入用 `on("clicked", fn)` 事件回调**，不要在主循环里自己采样触摸。
   回调里只改状态表，不要在回调里跑重逻辑。
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

## 完整性要求

只输出**能直接运行的完整脚本**：从 `require` 到主循环一次写完。
不要先给示意片段再给完整版，不要用 `-- ...` 或"其余同上"省略任何一段——
上位机会把你的代码原样上传执行，缺一段就是运行时报错。
如果预算紧张，就减少装饰性元素（云朵、眼睛、渐变），不要减少主循环和状态逻辑。

## 无输入设备时

如果规划里 `input_method` 是 `none`，做成**自动演示**：用固定策略驱动，
让画面自己动起来，但同样保持主循环常驻，不要让脚本自己退出。
不要生成一个等触摸但永远等不到的死循环。

---
{
  "name": "input-touch",
  "description": "用 lcd_touch 做触摸输入采样与手势判定的实现规范",
  "match": { "modules": ["lcd_touch"] },
  "priority": 20
}
---

触摸句柄必须从 `board_manager` 取，`lcd_touch` 的函数全都要传句柄。

```lua
local board_manager = require("board_manager")
local lcd_touch = require("lcd_touch")

local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")

-- 进入主循环前调用一次 sync，初始化 lcd_touch 模块的内部边沿状态。
lcd_touch.sync(touch_handle)

-- 主循环里每帧只调用 poll，不要再 sync。
local touch = lcd_touch.poll(touch_handle)
if touch.pressed then
    print("touch", touch.x, touch.y, touch.dx, touch.dy)
end
```

## 必守规则

1. **`poll` / `read` / `sync` 都必须传 `touch_handle`。** 漏掉句柄会报 `handle expected`。
2. **不要在主循环里每帧 `sync` 后再 `poll`。** `sync()` 会刷新模块内部缓存状态；如果紧接着 `poll()`，
   会抹掉 `just_pressed` / `just_released` / `dx` / `dy` 这类边沿和增量信息。正确做法是：
   获取 `touch_handle` 后、进入主循环前 `lcd_touch.sync(touch_handle)` 一次；主循环里只调用
   `lcd_touch.poll(touch_handle)`。
3. **`x` / `y` 只在 `pressed` 为真时有意义**，`read()` 返回的表在未按下时不含这两个字段，用之前先判 `pressed` 或 `just_pressed`，否则会拿到 nil 参与算术。
4. 用 `just_pressed` 做"点击一次"的判定，不要用 `pressed`——后者在整个按住期间都为真，会导致一次触摸被当成几十次。
5. 滑动/切水果/画轨迹这类连续坐标任务，用 `pressed` 期间的连续 `x/y` 采样，或用 `dx/dy` 做增量；
   释放手势/方向滑动可在 `just_released` 时结合按下起点判定。
6. 方向手势用 `dx` / `dy` 的符号判定，取绝对值大的那个轴作为主方向，并设一个最小阈值（比如 8 像素）过滤抖动。
7. 触摸轮询必须配 `lvgl.process_events(ms)` 或 `delay.delay_ms(ms)` 让出 CPU，纯忙等会饿死其他任务。

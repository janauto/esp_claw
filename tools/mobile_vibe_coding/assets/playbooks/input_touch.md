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

-- 主循环里每帧一次：先 sync 再 poll
lcd_touch.sync(touch_handle)
local touch = lcd_touch.poll(touch_handle)
if touch.just_pressed then
    print("press", touch.x, touch.y)
end
```

## 必守规则

1. **`poll` / `read` / `sync` 都必须传 `touch_handle`。** 漏掉句柄会报 `handle expected`。
2. **每帧先 `sync` 再 `poll`。** `poll` 给的是边沿信息（`just_pressed` / `just_released` / `dx` / `dy` / `held_ms`），不 sync 就拿不到本帧的新状态。
3. **`x` / `y` 只在 `pressed` 为真时有意义**，`read()` 返回的表在未按下时不含这两个字段，用之前先判 `pressed` 或 `just_pressed`，否则会拿到 nil 参与算术。
4. 用 `just_pressed` 做"点击一次"的判定，不要用 `pressed`——后者在整个按住期间都为真，会导致一次触摸被当成几十次。
5. 方向手势用 `dx` / `dy` 的符号判定，取绝对值大的那个轴作为主方向，并设一个最小阈值（比如 8 像素）过滤抖动。
6. 触摸轮询必须配 `delay.ms(...)`，纯忙等会饿死其他任务。

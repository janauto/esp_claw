---
{
  "name": "input-button",
  "description": "用 button 做实体按键输入的实现规范，含板级按键组（多键共用一路 ADC）",
  "match": { "modules": ["button"] },
  "priority": 20
}
---

实体按键有两种来源，先判断用哪种：

* **板级按键组**：板子上已经在 `board_devices.yaml` 里声明好的按键（常见于开发板的音量/模式键，多个键共用一路 ADC）。用 `button.open_group(设备名)`。
* **单个 GPIO 按键**：自己接在某个 GPIO 上的按键。用 `button.new(gpio_num, active_level)`。

需求里说"板子上的按键""音量键""开发板自带按键"时选前者；说"我接在 GPIO x 上的按键"时选后者。

## 板级按键组

```lua
local button = require("button")
local delay = require("delay")

local group = assert(button.open_group("adc_button_group"))

-- 一个回调覆盖整组；evt.label 告诉你按的是哪个键
button.on(group, "single_click", function(evt)
    print("按下: " .. tostring(evt.label))
end)

while true do
    button.dispatch()   -- 不调用它回调永远不会执行
    delay.delay_ms(10)
end
```

想知道这块板子有哪些键，用 `button.group_labels(group)` 打出来，不要凭猜测写 label 字符串。
只绑定其中一个键时用 `button.group_button(group, "SET")`（也可以传 1 开始的索引）。

## 单个 GPIO 按键

```lua
local handle = assert(button.new(0, 0))   -- gpio 0，低电平有效
button.on(handle, "single_click", function() print("clicked") end)
```

## 必守规则

1. **主循环里必须周期性调 `button.dispatch()`。** 按键事件在 C 回调里进队列，`dispatch()` 才把它们派发到 Lua 回调。不调用就一个回调都不会触发。
2. **`dispatch()` 必须配 `delay.delay_ms(...)`**（10ms 量级），纯忙等会饿死其他任务。
3. **只有板级按键组的事件才有 `label` 和 `index`。** `button.new` 建的单个按键没有这两个字段，用之前先判 `nil`，否则拼字符串会拿到 nil。
4. **`open_group` 的设备名要和 `board_devices.yaml` 里的 `name` 完全一致**（例如 `adc_button_group`），写错会返回 `nil, err`——用 `assert()` 包住，不要静默忽略。
5. **只有板级 YAML `events_cfg` 里置 `true` 的事件才会触发。** 常见开着的是 `press_down` / `press_up` / `single_click` / `double_click` / `long_press_start` / `long_press_up`；订阅了没开的事件不会报错，但永远不来回调，别把它当程序 bug 排查。
6. **判定"按了一次"用 `single_click`，不要用 `press_down` 计数**——后者在每次按下都触发，配合双击/长按时容易重复计数。
7. **不要对按键组的成员句柄调 `button.close`。** 组里的句柄由板级管理器持有，`button.close(group)` 只退订、不销毁；脚本退出前退订一次即可。
8. `button.on` 传组时会给每个键注册同一个回调，**不要为每个键写一遍 `on`**，除非各键行为确实不同。

# Lua Button

This module describes how to correctly use button when writing Lua scripts.

## How to call
- Import it with `local button = require("button")`
- Call `local handle = button.new(gpio_num [, active_level [, long_press_ms [, short_press_ms]]])` to create a GPIO button handle
- Call `local group = button.open_group(device_name)` to open a button device declared in `board_devices.yaml` (for example an `adc_multi` group where several keys share one ADC channel)
- Call `button.on(target, event, callback)` to subscribe to a button event; `target` may be a single handle or a whole group
- Supported event names include `press_down`, `press_up`, `press_repeat`, `press_repeat_done`, `single_click`, `double_click`, `multiple_click`, `long_press_start`, `long_press_hold`, `long_press_up`, and `press_end`
- Call `button.off(target [, event])` to remove a subscription or clear callbacks
- Call `button.dispatch()` to poll and dispatch pending button events
- Call `button.get_key_level(handle)` to read the current key level
- Call `button.close(target)` when the handle or group is no longer needed

## Board button groups
- `button.open_group(device_name)` returns a group object, or `nil, err`. The device is initialized on demand if it was not brought up yet.
- `button.group_size(group)` returns the number of keys in the group.
- `button.group_labels(group)` returns an array of the `button_labels` values declared in YAML, in hardware order.
- `button.group_button(group, index_or_label)` returns a single button handle, addressed either by 1-based index or by its YAML label.
- Subscribing on the group registers the same callback for every key. The callback tells you which key fired via the `label` and `index` fields.
- Handles that belong to a board group are owned by the board manager. `button.close(group)` only drops the subscriptions; it does not delete the underlying buttons.

## Return values and events
- `button.new(...)` returns a handle or `nil, err`.
- `button.close(target)`, `button.on(target, event, callback)`, and `button.off(target[, event])` return `true` or `nil, err`.
- `button.dispatch()` returns the number of dispatched callback events.
- Callback events are tables with `handle`, `event`, `repeat_count`, and `pressed_time_ms`.
- Events coming from a board group additionally carry `label` (the YAML `button_labels` entry) and `index` (1-based position in the group).

## Example
```lua
local button = require("button")

local handle = button.new(0, 0)
button.on(handle, "single_click", function(evt)
  print(evt.event, evt.repeat_count)
end)

button.dispatch()
button.close(handle)
```

## Example: ADC button group
For a board that declares four keys on one ADC channel:

```yaml
  - name: adc_button_group
    type: button
    sub_type: adc_multi
    config:
      button_num: 4
      max_voltage: 2000
      voltage_range: [380, 820, 1340, 1870]
      button_labels: ["VOLUME_UP", "VOLUME_DOWN", "MODE", "SET"]
```

```lua
local button = require("button")

local group = assert(button.open_group("adc_button_group"))

-- One callback for all four keys; evt.label says which one was pressed.
button.on(group, "single_click", function(evt)
  print("pressed: " .. tostring(evt.label))   -- VOLUME_UP / VOLUME_DOWN / MODE / SET
end)

-- Or bind a single key by label.
local set_key = assert(button.group_button(group, "SET"))
button.on(set_key, "long_press_start", function()
  print("SET held down")
end)

button.dispatch()
button.close(group)
```

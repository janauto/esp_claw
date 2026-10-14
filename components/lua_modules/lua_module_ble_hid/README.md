# Lua BLE HID

This module describes how to correctly use BLE HID when writing Lua scripts.
It exposes the device as a composite BLE HID peripheral with media control,
keyboard, and mouse input reports.

## How to call
- Import it with `local ble_hid = require("ble_hid")`
- Call `ble_hid.init([{ name = "esp-claw-hid" }])` before advertising or sending reports
- Call `ble_hid.start([{ name = "esp-claw-hid" }])` to start HID advertising
- Pair from the host operating system Bluetooth settings with the advertised name
- Call `ble_hid.status()` to read `{ initialized, advertising, connected, encrypted, notify_enabled, bonded, ready }`
- Call `ble_hid.describe()` to read report ids and text input limits
- Call `ble_hid.media(key [, gesture])` to send a Consumer Control report
- Call `ble_hid.key(key)` to press and release one keyboard key
- Call `ble_hid.combo(key_or_modifier, ...)` to press a shortcut such as `CTRL+C`
- Call `ble_hid.text(text)` to type printable ASCII text on a US keyboard layout
- Call `ble_hid.mouse_move(dx, dy [, wheel [, pan]])` to move the mouse
- Call `ble_hid.mouse_scroll(wheel [, pan])` to scroll vertically or horizontally
- Call `ble_hid.mouse_button(button [, gesture])` to click, press, or release a mouse button
- Call `ble_hid.release_all()` before stopping or after interrupted pointer/key actions
- Call `ble_hid.stop()` to stop advertising, and `ble_hid.deinit()` to release the HID stack

`init` and `start` accept an optional `name` field. The name length must be 29
bytes or less. The default name is `esp-claw-hid`.

## Example
```lua
local ble_hid = require("ble_hid")

local ok, err = ble_hid.init({ name = "esp-claw-hid" })
if not ok then
    error(err)
end

ok, err = ble_hid.start({ name = "esp-claw-hid" })
if not ok then
    error(err)
end

print("Pair with esp-claw-hid from the host Bluetooth settings")

local status = ble_hid.status()
print("connected:", status.connected, "bonded:", status.bonded)

ble_hid.media("play_pause")
ble_hid.media("volume_up")
ble_hid.key("ENTER")
ble_hid.combo("CTRL", "C")
ble_hid.text("hello")
ble_hid.mouse_move(20, 0)
ble_hid.mouse_button("left", "click")
ble_hid.mouse_scroll(-3, 0)

ble_hid.release_all()
ble_hid.stop()
ble_hid.deinit()
```

## Supported Input

Media keys:
- `volume_up`
- `volume_down`
- `play_pause`
- `next_track`
- `previous_track`
- `mute`

Media gestures:
- `single`
- `double`
- `long`

Keyboard keys include:
- `A` through `Z`
- `0` through `9`
- `ENTER`, `ESC`, `BACKSPACE`, `TAB`, `SPACE`
- `MINUS`, `EQUAL`, `LEFT_BRACKET`, `RIGHT_BRACKET`, `BACKSLASH`
- `SEMICOLON`, `QUOTE`, `GRAVE`, `COMMA`, `PERIOD`, `SLASH`
- `CAPS_LOCK`, `F1` through `F12`
- `PRINT_SCREEN`, `SCROLL_LOCK`, `PAUSE`, `INSERT`, `HOME`
- `PAGE_UP`, `DELETE`, `END`, `PAGE_DOWN`
- `RIGHT`, `LEFT`, `DOWN`, `UP`

Keyboard modifiers for `combo`:
- `CTRL`, `CONTROL`
- `SHIFT`
- `ALT`, `OPTION`
- `GUI`, `COMMAND`, `CMD`, `META`
- `RIGHT_CTRL`, `RIGHT_SHIFT`, `RIGHT_ALT`, `RIGHT_GUI`

Mouse buttons:
- `left`
- `right`
- `middle`

Mouse button gestures:
- `click`
- `down`
- `up`

`ble_hid.text(text)` simulates basic ASCII on a standard US keyboard layout. It
supports letters, digits, common printable punctuation, space, newline, and tab.
It does not support Unicode, IME input, emoji, dead keys, or non-US layout
correction.

## Return Values

Most operations return `true` on success. Runtime failures return `nil, err`.
Argument validation errors raise Lua errors.

Typical failures are:
- `HID not initialized`: call `ble_hid.init()` first
- `not connected`: pair and connect from the host before sending reports
- `unsupported ...`: use one of the supported key, modifier, media, button, or gesture names

## Introspection

`ble_hid.describe()` returns:
- `consumer_report_id`
- `keyboard_report_id`
- `mouse_report_id`
- `text_scope`
- `text_unicode`
### Shared-host media pairing fix (2026-10-14)

In attached mode, input Report Reference descriptors map report IDs to actual
ATT handles. An input report subscription selects the HID peer and initiates
SMP; encryption alone does not select bridge clients. Report notifications go
to that encrypted subscriber explicitly. Bridge subscriptions and disconnects
do not clear the selected phone's state. This fixes the observed iPhone state
`notify_enabled=true`, `connected=false`, `encrypted=false` on 26261e3d.

Validation: the esp32_s31_korvo1 IDF 6.1 build and 15 extracted C function host
checks passed. The a97cb556 app image was flashed at 0x20000 with device hash
verification on 2026-10-15. Boot ELF prefix 5d4eb3211 and report handles
37/41/45 were read back; the installed Lua application remains on SD.
Actual iPhone pairing and media results remain separate acceptance gates.
macOS CoreBluetooth also subscribes to HID when using the bridge; subscription
does not identify a phone platform. A Mac transfer started SMP and timed out;
the Mac was released before asking for iPhone-only pairing. Simultaneous host
acceptance has not passed.

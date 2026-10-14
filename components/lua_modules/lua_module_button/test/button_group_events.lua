-- Board button group demo (polling). Reads a button device from board_devices.yaml
-- and reports which key fired via evt.label. Optional args: device, duration_ms, poll_ms.
-- Pattern: open hardware in run(), xpcall(run) + cleanup() + rethrow — copy this skeleton, not ad-hoc early returns.
local btn   = require("button")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local cfg_device  = type(a.device) == "string" and a.device or "adc_button_group"
local duration_ms = int_arg("duration_ms", 30000)
local poll_ms     = int_arg("poll_ms", 10)

local group

local function cleanup()
    if group then
        pcall(btn.off, group)
        pcall(btn.close, group)
        group = nil
    end
end

local function run()
    local g, gerr = btn.open_group(cfg_device)
    if not g then
        error("[button_group_demo] button.open_group failed: " .. tostring(gerr))
    end
    group = g

    local size = btn.group_size(group)
    local labels = btn.group_labels(group)
    print(string.format("[button_group_demo] device '%s' has %d keys: %s",
        cfg_device, size, table.concat(labels, ", ")))

    -- One callback per event covers every key in the group; evt.label identifies it.
    local function report(kind)
        return function(evt)
            print(string.format("[button_group_demo] %-16s key=%s index=%s pressed_time=%sms",
                kind, tostring(evt.label), tostring(evt.index), tostring(evt.pressed_time_ms)))
        end
    end

    -- Only events enabled in the YAML events_cfg will actually fire.
    for _, ev in ipairs({
        "press_down",
        "press_up",
        "single_click",
        "double_click",
        "long_press_start",
        "long_press_up",
    }) do
        local ok, err = btn.on(group, ev, report(ev))
        if not ok then
            print("[button_group_demo] ERROR: on(" .. ev .. "): " .. tostring(err))
        end
    end

    print(string.format(
        "[button_group_demo] listening for %d ms (poll=%dms), press the buttons...",
        duration_ms, poll_ms))

    local iters = math.max(1, math.floor(duration_ms / poll_ms))
    for _ = 1, iters do
        btn.dispatch()
        delay.delay_ms(poll_ms)
    end
end

local run_ok, run_err = xpcall(run, debug.traceback)
cleanup()
print("[button_group_demo] done")
if not run_ok then
    error(run_err)
end

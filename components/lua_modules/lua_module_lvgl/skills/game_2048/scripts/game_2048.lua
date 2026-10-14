--[[
Description: 2048。LVGL 画面 + 触摸滑动 + 实体按键。

    滑动棋盘        -> 上/下/左/右移动方块，相同数字合并
    屏幕上的 新游戏 -> 重开一局
    屏幕上的 退出 / MODE -> 退出，回到启动本脚本的那个界面

画面走 LVGL，适配器旋转屏幕时（例如竖屏 480x800）布局跟着 scr:get_size()
走，触摸坐标也是同一套，不用脚本自己转。

触摸：LVGL 的 Lua 事件不带坐标，滑动方向只能自己算，所以触摸全部从
lcd_touch 轮询里来，按钮也是自己做命中判断，不再 indev_register 第二个
LVGL 输入设备——同一个 GT1151 被多个任务读时，读走数据的一方会把点清掉，
另一方偶尔读到空，松手判定因此要连续几次读空才算。

退出：lvgl.init() 开的是带 RESTORE_DEFAULT_ON_RELEASE 的独占显示会话，
      lvgl.deinit() 会把屏幕交还给启动本脚本的界面。释放显示之前先退订按键
      ——iot_button_unregister_cb() 会清掉 single_click 上的全部回调（含首页
      那一份），而首页是在显示会话释放时才重新订阅的，顺序反了退出后首页
      按键就失灵。
--]]
local bm = require("board_manager")
local lvgl = require("lvgl")
local delay = require("delay")
local system_ok, system = pcall(require, "system")
local touch_ok, lcd_touch = pcall(require, "lcd_touch")
local audio_ok, audio = pcall(require, "audio")
local button_ok, button = pcall(require, "button")

local a = type(args) == "table" and args or {}

local function int_arg(name, default)
    local value = a[name]
    if type(value) == "number" then
        return math.floor(value)
    end
    return default
end

local function now_ms()
    if system_ok and system and system.millis then
        return system.millis()
    end
    return math.floor(os.clock() * 1000)
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local RUN_TIME_MS = int_arg("run_time_ms", 1800000)
local FRAME_MS = 16
local SWIPE_THRESHOLD = 44
-- 连续这么多次读不到点才算松手，约 50ms，滑动手势远比这长
local RELEASE_POLLS = 3
local SOUND_VOLUME = 84
local UAC_FLUSH_PCM_BYTES = 4000

-- 配色沿用原来的 2048，首页图标（ui_icon_2048.c）也是照这套取的色
local C_BG = "#F8F5EF"
local C_PANEL = "#9D8F84"
local C_PANEL_DOWN = "#7F736A"
local C_GRID = "#BBAFA5"
local C_EMPTY = "#CDC4BB"
local C_SHADOW = "#DED6CD"
local C_TEXT = "#FFFAF4"
local C_SUBTEXT = "#EFE8E0"
local C_DARK = "#5B544D"
local C_GOLD = "#EDC22E"
local C_RED = "#F65E3B"

local TILE_COLORS = {
    [2] = "#EEE4DA",
    [4] = "#EDE0C8",
    [8] = "#F2B179",
    [16] = "#F59563",
    [32] = "#F67C5F",
    [64] = "#F65E3B",
    [128] = "#EDCF72",
    [256] = "#EDCC61",
    [512] = "#EDC850",
    [1024] = "#EDC53F",
    [2048] = "#EDC22E",
}
local TILE_BEYOND = "#3A8ACD"

local function tile_color(value)
    return TILE_COLORS[value] or TILE_BEYOND
end

local function tile_text_color(value)
    if value <= 4 then return C_DARK end
    return C_TEXT
end

---------------------------------------------------------------- 字号

-- Lua 的 lvgl 每次运行只有一个默认字号（font_load 只认 DATA 分区里的字体），
-- 所以整个界面用同一个字号，按棋盘格子大小挑。棋盘边长等于短边减去边距，
-- 横竖屏都一样，所以 lvgl.init 之前就能按面板短边算出来。
local function pick_font_size()
    local ok, _, _, w, h = pcall(bm.get_display_lcd_params, "display_lcd")
    local short = 480
    if ok and type(w) == "number" and type(h) == "number" and w > 0 and h > 0 then
        short = math.min(w, h)
    end
    if short >= 440 then return 28 end
    if short >= 320 then return 22 end
    return 18
end

local FONT_SIZE = pick_font_size()

---------------------------------------------------------------- 初始化

-- 先要显示会话再拿触摸：board_manager 的 lcd_touch 设备只有在显示起来之后
-- 才有 handle。
local ok, err = pcall(lvgl.init, {
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
    font_size = FONT_SIZE,
})
if not ok then
    print("[game_2048] ERROR: lvgl.init failed: " .. tostring(err))
    return
end

local key_group = nil
local audio_output = nil

local function cleanup()
    if key_group then
        pcall(button.off, key_group)
        pcall(button.close, key_group)
        key_group = nil
    end
    if audio_output then
        pcall(audio_output.close, audio_output)
        audio_output = nil
    end
    pcall(lvgl.deinit)
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C_BG, bg_opa = 255 })
scr:set_scroll({ dir = "none", scrollbar = "off" })

local scr_w, scr_h = scr:get_size()
if not scr_w or scr_w <= 0 or not scr_h or scr_h <= 0 then
    print("[game_2048] ERROR: invalid screen size")
    cleanup()
    return
end

if not touch_ok then
    print("[game_2048] ERROR: require(lcd_touch) failed")
    cleanup()
    return
end

local touch_handle, touch_err = bm.get_lcd_touch_handle("lcd_touch")
if not touch_handle then
    print("[game_2048] ERROR: get_lcd_touch_handle(lcd_touch) failed: " .. tostring(touch_err))
    cleanup()
    return
end

local synced, sync_err = pcall(lcd_touch.sync, touch_handle)
if not synced then
    print("[game_2048] ERROR: lcd_touch.sync failed: " .. tostring(sync_err))
    cleanup()
    return
end

---------------------------------------------------------------- 音效

local output_codec, board_output_rate, output_channels, output_bits =
    bm.get_audio_codec_output_params("audio_dac")
local OUTPUT_SAMPLE_RATE = int_arg("sample_rate_hz", board_output_rate or 16000)

local sfx = {}
local pending_sfx = nil

local function build_tone(freq_hz, duration_ms, amp, wave)
    local rate = OUTPUT_SAMPLE_RATE
    local frames = math.floor(rate * duration_ms / 1000)
    if frames <= 0 then
        return string.rep("\0", UAC_FLUSH_PCM_BYTES)
    end

    local chunks = {}
    local phase = 0
    local step = 2 * math.pi * freq_hz / rate
    for i = 1, frames do
        local raw
        if wave == "square" then
            raw = math.sin(phase) >= 0 and 1 or -1
        else
            raw = math.sin(phase)
        end
        local fade = 1
        if i > frames * 0.72 then
            fade = (frames - i) / math.max(1, frames * 0.28)
        end
        local sample = math.floor(raw * amp * fade)
        phase = phase + step
        if sample > 32767 then sample = 32767 end
        if sample < -32768 then sample = -32768 end
        local u = sample < 0 and sample + 65536 or sample
        chunks[i] = string.char(u % 256, math.floor(u / 256) % 256)
    end

    local pcm = table.concat(chunks)
    if #pcm < UAC_FLUSH_PCM_BYTES then
        pcm = pcm .. string.rep("\0", UAC_FLUSH_PCM_BYTES - #pcm)
    end
    return pcm
end

local function init_audio()
    if not audio_ok or not output_codec then
        print("[game_2048] WARN: audio unavailable")
        return
    end

    local out_ok, out = pcall(function()
        return audio.new_output({
            codec = output_codec,
            sample_rate = OUTPUT_SAMPLE_RATE,
            channels = output_channels,
            bits = output_bits,
            volume = SOUND_VOLUME,
        })
    end)
    if not out_ok or not out then
        print("[game_2048] WARN: audio init failed: " .. tostring(out))
        return
    end

    audio_output = out
    sfx.start = build_tone(660, 80, 12000, "square")
    sfx.move = build_tone(360, 48, 9000, "square")
    sfx.merge = build_tone(920, 80, 13000, "square")
    sfx.block = build_tone(150, 70, 9000, "square")
    sfx.over = build_tone(180, 240, 11000, "square")
    pending_sfx = sfx.start
end

local function request_sfx(name)
    if sfx[name] then
        pending_sfx = sfx[name]
    end
end

local function drain_sfx()
    if not pending_sfx or not audio_output then
        return
    end
    local pcm = pending_sfx
    local wrote, write_err = audio_output:write(pcm)
    if wrote or (write_err and not tostring(write_err):find("busy", 1, true)) then
        pending_sfx = nil
    end
end

init_audio()

---------------------------------------------------------------- 实体按键

-- MODE 退出，和 meeting_presenter.lua、snake_game.lua 一个含义。
-- 键位靠 YAML button_labels 认，找不到时按硬件顺序兜底。
local KEY_ORDER = { "VOLUME_UP", "VOLUME_DOWN", "MODE", "SET" }
local running = true

local function quit()
    running = false
end

local function init_keys()
    if not button_ok or not button then
        print("[game_2048] WARN: require(button) failed, MODE exit unavailable")
        return
    end

    local opened, group = pcall(button.open_group, "adc_button_group")
    if not opened or not group then
        print("[game_2048] WARN: open_group(adc_button_group) failed: " .. tostring(group))
        return
    end
    key_group = group

    local ACTIONS = { MODE = quit }
    local labels = button.group_labels(group) or {}
    local by_label = {}
    for i, name in ipairs(labels) do
        by_label[name] = ACTIONS[name] or ACTIONS[KEY_ORDER[i]]
    end

    -- single_click 而不是 press_down：抖动一下就会误退出
    local subscribed, sub_err = pcall(button.on, group, "single_click", function(evt)
        local action = evt.label and by_label[evt.label]
        if action then
            action()
        end
    end)
    if not subscribed then
        print("[game_2048] WARN: button.on failed: " .. tostring(sub_err))
        pcall(button.close, group)
        key_group = nil
    end
end

init_keys()

---------------------------------------------------------------- 布局

-- 竖屏：标题和计分在上，棋盘居中，提示和两个按钮在下，整摞上下居中。
-- 横屏：棋盘在左，其余一列摆在右边。两种都按 scr:get_size() 算。
local MARGIN = 16
local GAP = 24
local portrait = scr_h >= scr_w

local avail_w = scr_w - MARGIN * 2
local header_h = clamp(math.floor(math.min(scr_w, scr_h) * 0.18), 64, 88)
local hint_h = FONT_SIZE + 10
local btn_h = clamp(FONT_SIZE * 2 + 16, 52, 72)

local board_side
if portrait then
    board_side = math.min(avail_w, scr_h - MARGIN * 2 - header_h - hint_h - btn_h - GAP * 3)
else
    board_side = math.min(scr_h - MARGIN * 2, math.floor(scr_w * 0.58))
end
local cell_gap = math.max(8, math.floor(board_side * 0.027))
local cell = math.floor((board_side - cell_gap * 5) / 4)
board_side = cell * 4 + cell_gap * 5

local board_x, board_y
local header_x, header_y, header_w
local hint_x, hint_y, hint_w
local btn_y, btn_x, btn_w, btn_gap

if portrait then
    local total = header_h + GAP + board_side + GAP + hint_h + GAP + btn_h
    local top = math.max(MARGIN, math.floor((scr_h - total) / 2))
    board_x = math.floor((scr_w - board_side) / 2)
    header_x, header_y, header_w = board_x, top, board_side
    board_y = top + header_h + GAP
    hint_x, hint_y, hint_w = board_x, board_y + board_side + GAP, board_side
    btn_y = hint_y + hint_h + GAP
    btn_x = board_x
    btn_gap = GAP
    btn_w = math.floor((board_side - btn_gap) / 2)
else
    board_x = MARGIN
    board_y = math.floor((scr_h - board_side) / 2)
    local col_x = board_x + board_side + GAP
    local col_w = scr_w - col_x - MARGIN
    local total = header_h + GAP + hint_h + GAP + btn_h
    local top = board_y + math.max(0, math.floor((board_side - total) / 2))
    header_x, header_y, header_w = col_x, top, col_w
    hint_x, hint_y, hint_w = col_x, top + header_h + GAP, col_w
    btn_y = hint_y + hint_h + GAP
    btn_x = col_x
    btn_gap = 16
    btn_w = math.floor((col_w - btn_gap) / 2)
end

local function no_scroll(obj)
    obj:set_scroll({ dir = "none", scrollbar = "off" })
    return obj
end

local function box(parent, x, y, w, h, color, radius)
    local obj = lvgl.container(parent, { x = x, y = y, w = w, h = h })
    obj:set_style({ bg_color = color, bg_opa = 255, radius = radius, border_width = 0, pad = 0 })
    return no_scroll(obj)
end

---------------------------------------------------------------- 顶部：标题 + 计分

-- set_style 没有 text_align：label 不给宽度时会收缩到文字大小，再用 align
-- 摆到卡片中间，视觉上就是文字居中。
local title = box(scr, header_x, header_y, header_h, header_h, C_GOLD, 8)
lvgl.label(title, { text = "2048", align = "center", text_color = C_TEXT })

local card_gap = 12
local card_x = header_x + header_h + card_gap
local card_w = math.floor((header_x + header_w - card_x - card_gap) / 2)

local function make_card(x, caption)
    local card = box(scr, x, header_y, card_w, header_h, C_PANEL, 8)
    lvgl.label(card, { text = caption, align = "top_mid", y = 6, text_color = C_SUBTEXT })
    return lvgl.label(card, { text = "0", align = "bottom_mid", y = -6, text_color = C_TEXT })
end

local score_label = make_card(card_x, "分数")
local best_label = make_card(card_x + card_w + card_gap, "最高")

---------------------------------------------------------------- 棋盘

box(scr, board_x + 3, board_y + 4, board_side, board_side, C_SHADOW, 12)
local board_obj = box(scr, board_x, board_y, board_side, board_side, C_GRID, 12)

-- 格子在棋盘容器里的坐标（容器内边距为 0），x/y 都从 1 起
local function cell_pos(x, y)
    return cell_gap + (x - 1) * (cell + cell_gap), cell_gap + (y - 1) * (cell + cell_gap)
end

for y = 1, 4 do
    for x = 1, 4 do
        local px, py = cell_pos(x, y)
        box(board_obj, px, py, cell, cell, C_EMPTY, 7)
    end
end

-- 16 个方块对象常驻，换数字只改颜色和文字，空格子透明
local tiles = {}
for y = 1, 4 do
    tiles[y] = {}
    for x = 1, 4 do
        local px, py = cell_pos(x, y)
        local obj = box(board_obj, px, py, cell, cell, C_EMPTY, 7)
        obj:set_style({ opa = 0 })
        local label = lvgl.label(obj, { text = "", align = "center", text_color = C_DARK })
        tiles[y][x] = { obj = obj, label = label }
    end
end

local overlay = nil

---------------------------------------------------------------- 提示 + 按钮

local hint_box = lvgl.container(scr, { x = hint_x, y = hint_y, w = hint_w, h = hint_h })
hint_box:set_style({ bg_opa = 0, border_width = 0, pad = 0 })
no_scroll(hint_box)
local hint_label = lvgl.label(hint_box, { text = "", align = "center", text_color = C_DARK })

local function make_button(x, text)
    local obj = box(scr, x, btn_y, btn_w, btn_h, C_PANEL, 10)
    lvgl.label(obj, { text = text, align = "center", text_color = C_TEXT })
    return { obj = obj, x = x, y = btn_y, w = btn_w, h = btn_h }
end

local new_btn = make_button(btn_x, "新游戏")
local exit_btn = make_button(btn_x + btn_w + btn_gap, "退出")
local BUTTONS = { { btn = new_btn, action = "new" }, { btn = exit_btn, action = "quit" } }

local function hit(btn, x, y)
    return x >= btn.x and x < btn.x + btn.w and y >= btn.y and y < btn.y + btn.h
end

local function set_pressed(btn, pressed)
    btn.obj:set_style({ bg_color = pressed and C_PANEL_DOWN or C_PANEL })
end

---------------------------------------------------------------- 游戏状态

local board = {}
local score = 0
local best = 0
local game_over = false
local won = false          -- 已经合出过 2048
local won_overlay = false  -- 「2048!」提示还挂着，下一次滑动只关提示

math.randomseed((os.time() or 1) + scr_w * 17 + scr_h * 31 + now_ms())

local function empty_board()
    board = {}
    for y = 1, 4 do
        board[y] = { 0, 0, 0, 0 }
    end
end

local function empty_cells()
    local cells = {}
    for y = 1, 4 do
        for x = 1, 4 do
            if board[y][x] == 0 then
                cells[#cells + 1] = { x = x, y = y }
            end
        end
    end
    return cells
end

local function add_random_tile()
    local cells = empty_cells()
    if #cells == 0 then return nil end
    local pick = cells[math.random(1, #cells)]
    board[pick.y][pick.x] = math.random(1, 10) == 10 and 4 or 2
    return pick
end

local function has_moves()
    if #empty_cells() > 0 then return true end
    for y = 1, 4 do
        for x = 1, 4 do
            local v = board[y][x]
            if x < 4 and board[y][x + 1] == v then return true end
            if y < 4 and board[y + 1][x] == v then return true end
        end
    end
    return false
end

local function coords_for(direction, line)
    local coords = {}
    for i = 1, 4 do
        local x, y
        if direction == "left" then
            x, y = i, line
        elseif direction == "right" then
            x, y = 5 - i, line
        elseif direction == "up" then
            x, y = line, i
        else
            x, y = line, 5 - i
        end
        coords[i] = { x = x, y = y }
    end
    return coords
end

-- 返回 moved 和合并出的格子列表（用来做弹一下的动画）
local function slide(direction)
    local next_board = {}
    for y = 1, 4 do
        next_board[y] = { 0, 0, 0, 0 }
    end
    local merged_cells = {}
    local moved = false

    for line = 1, 4 do
        local coords = coords_for(direction, line)
        local target_i = 1
        local last_target = nil
        local last_merged = false

        for scan_i = 1, 4 do
            local src = coords[scan_i]
            local value = board[src.y][src.x]
            if value ~= 0 then
                if last_target and not last_merged and next_board[last_target.y][last_target.x] == value then
                    next_board[last_target.y][last_target.x] = value * 2
                    last_merged = true
                    score = score + value * 2
                    if score > best then best = score end
                    merged_cells[#merged_cells + 1] = last_target
                    moved = true
                else
                    local dst = coords[target_i]
                    next_board[dst.y][dst.x] = value
                    last_target = dst
                    last_merged = false
                    moved = moved or src.x ~= dst.x or src.y ~= dst.y
                    target_i = target_i + 1
                end
            end
        end
    end

    if not moved then
        return false, {}
    end
    board = next_board
    return true, merged_cells
end

---------------------------------------------------------------- 画面

-- 方块弹一下：新方块从小长大，合并的方块先放大再回落。几帧就完，
-- 在主循环里逐帧改尺寸，结束时摆回原位。
local POP_FRAMES = 6
local pops = {}

local function place_tile(t, x, y, scale)
    local size = math.floor(cell * scale)
    local px, py = cell_pos(x, y)
    local off = math.floor((cell - size) / 2)
    t.obj:set_size(size, size)
    t.obj:set_pos(px + off, py + off)
end

local function paint_tile(x, y)
    local t = tiles[y][x]
    local value = board[y][x]
    if value == 0 then
        t.obj:set_style({ opa = 0 })
        return
    end
    t.obj:set_style({ opa = 255, bg_color = tile_color(value) })
    t.label:set_text(tostring(value))
    t.label:set_style({ text_color = tile_text_color(value) })
end

local function start_pop(x, y, kind)
    pops[#pops + 1] = { x = x, y = y, kind = kind, frame = 0 }
end

local function step_pops()
    if #pops == 0 then return end
    local still = {}
    for _, p in ipairs(pops) do
        p.frame = p.frame + 1
        local t = tiles[p.y][p.x]
        local k = p.frame / POP_FRAMES
        if k >= 1 then
            place_tile(t, p.x, p.y, 1)
        else
            local scale
            if p.kind == "new" then
                scale = 0.5 + 0.5 * k
            else
                scale = 1 + 0.12 * (1 - math.abs(2 * k - 1))
            end
            place_tile(t, p.x, p.y, scale)
            still[#still + 1] = p
        end
    end
    pops = still
end

local function clear_pops()
    for _, p in ipairs(pops) do
        place_tile(tiles[p.y][p.x], p.x, p.y, 1)
    end
    pops = {}
end

local function hide_overlay()
    if overlay and overlay:is_valid() then
        overlay:delete()
    end
    overlay = nil
end

local function show_overlay(title_text, sub_text, color)
    hide_overlay()
    local w = math.floor(board_side * 0.8)
    local h = FONT_SIZE * 2 + 96
    overlay = lvgl.container(board_obj, { w = w, h = h, align = "center" })
    overlay:set_style({
        bg_color = C_BG,
        bg_opa = 235,
        radius = 12,
        border_color = color,
        border_width = 4,
        pad = 0,
    })
    no_scroll(overlay)
    lvgl.label(overlay, { text = title_text, align = "top_mid", y = 28, text_color = color })
    lvgl.label(overlay, { text = sub_text, align = "bottom_mid", y = -28, text_color = C_DARK })
end

local function update_hud()
    score_label:set_text(tostring(score))
    best_label:set_text(tostring(best))
    if game_over then
        hint_label:set_text("没有可走的了")
    else
        hint_label:set_text("滑动合并相同数字")
    end
end

local function paint_all()
    for y = 1, 4 do
        for x = 1, 4 do
            paint_tile(x, y)
        end
    end
    update_hud()
end

local function reset_game()
    clear_pops()
    hide_overlay()
    empty_board()
    score = 0
    game_over = false
    won = false
    won_overlay = false
    local first = add_random_tile()
    local second = add_random_tile()
    paint_all()
    if first then start_pop(first.x, first.y, "new") end
    if second then start_pop(second.x, second.y, "new") end
    request_sfx("start")
end

local function reached_2048()
    for y = 1, 4 do
        for x = 1, 4 do
            if board[y][x] >= 2048 then return true end
        end
    end
    return false
end

local function on_swipe(direction)
    if game_over then
        reset_game()
        return
    end
    if won_overlay then
        won_overlay = false
        hide_overlay()
        return
    end

    local moved, merged_cells = slide(direction)
    if not moved then
        if has_moves() then
            request_sfx("block")
        else
            game_over = true
            show_overlay("游戏结束", "滑动任意方向重开", C_RED)
            update_hud()
            request_sfx("over")
        end
        return
    end

    clear_pops()
    local spawned = add_random_tile()
    paint_all()
    for _, c in ipairs(merged_cells) do
        start_pop(c.x, c.y, "merge")
    end
    if spawned then
        start_pop(spawned.x, spawned.y, "new")
    end

    if not won and reached_2048() then
        won = true
        won_overlay = true
        show_overlay("2048!", "继续滑动冲更高分", C_GOLD)
        print("[game_2048] reached 2048, score=" .. score)
    end

    if not has_moves() then
        game_over = true
        won_overlay = false
        show_overlay("游戏结束", "滑动任意方向重开", C_RED)
        update_hud()
        request_sfx("over")
        print("[game_2048] game over, score=" .. score)
    elseif #merged_cells > 0 then
        request_sfx("merge")
    else
        request_sfx("move")
    end
end

---------------------------------------------------------------- 触摸

-- 手指一移动超过阈值就出方向，不等松手，手感更跟手；一次按下只出一次。
-- 没移动够就松手算点击，点在按钮上才有动作。
local touch = { down = false, fired = false, sx = 0, sy = 0, lx = 0, ly = 0, miss = 0, btn = nil }

local function direction_of(dx, dy)
    if math.abs(dx) < SWIPE_THRESHOLD and math.abs(dy) < SWIPE_THRESHOLD then
        return nil
    end
    if math.abs(dx) > math.abs(dy) then
        return dx > 0 and "right" or "left"
    end
    return dy > 0 and "down" or "up"
end

local function button_at(x, y)
    for _, entry in ipairs(BUTTONS) do
        if hit(entry.btn, x, y) then
            return entry
        end
    end
    return nil
end

-- 返回 nil（无动作）、"error"、"quit"、"new" 或方向
local function poll_touch()
    local polled, info = pcall(lcd_touch.poll, touch_handle)
    if not polled then
        print("[game_2048] ERROR: lcd_touch.poll failed: " .. tostring(info))
        return "error"
    end

    if info.pressed then
        local x, y = info.x or 0, info.y or 0
        touch.miss = 0
        if not touch.down then
            touch.down = true
            touch.fired = false
            touch.sx, touch.sy = x, y
            touch.btn = button_at(x, y)
            if touch.btn then
                set_pressed(touch.btn.btn, true)
            end
        end
        touch.lx, touch.ly = x, y
        if not touch.fired and not touch.btn then
            local direction = direction_of(x - touch.sx, y - touch.sy)
            if direction then
                touch.fired = true
                return direction
            end
        end
        return nil
    end

    if not touch.down then
        return nil
    end
    touch.miss = touch.miss + 1
    if touch.miss < RELEASE_POLLS then
        return nil
    end

    touch.down = false
    local entry = touch.btn
    touch.btn = nil
    if entry then
        set_pressed(entry.btn, false)
        if hit(entry.btn, touch.lx, touch.ly) then
            return entry.action
        end
    end
    return nil
end

---------------------------------------------------------------- 主循环

reset_game()
scr:load()
print(string.format("[game_2048] ready screen=%dx%d board=%d cell=%d font=%d run_ms=%d",
    scr_w, scr_h, board_side, cell, FONT_SIZE, RUN_TIME_MS))
print("[game_2048] swipe on the board to slide tiles")

local run_ok, run_err = xpcall(function()
    local start = now_ms()
    while running and now_ms() - start < RUN_TIME_MS do
        if key_group then
            button.dispatch()
            if not running then break end
        end
        drain_sfx()
        local action = poll_touch()
        if action == "error" then
            break
        elseif action == "quit" then
            running = false
        elseif action == "new" then
            reset_game()
        elseif action then
            on_swipe(action)
        end
        step_pops()
        lvgl.process_events(0)
        delay.delay_ms(FRAME_MS)
    end
end, debug.traceback)

-- 顺序要紧：先退订按键再关显示会话，理由见文件头
hide_overlay()
cleanup()
if not run_ok then
    print("[game_2048] ERROR: " .. tostring(run_err))
end
print("[game_2048] done, score=" .. score)

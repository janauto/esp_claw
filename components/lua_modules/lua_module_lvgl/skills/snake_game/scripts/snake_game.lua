--[[
Description: 贪吃蛇。LVGL 画板 + 屏幕方向键 + 实体按键。

    VOLUME_UP / VOLUME_DOWN -> 上 / 下（没有触摸的板子上用来转向）
    MODE                    -> 退出，回到启动本脚本的那个界面
    屏幕上的 上/下/左/右    -> 转向；结束后任意方向键重开一局

穿墙模式：从一边出去，从对面回来。

退出：lvgl.init() 开的是带 RESTORE_DEFAULT_ON_RELEASE 的独占显示会话，
      所以 lvgl.deinit() 会把屏幕交还给启动本脚本的界面。释放显示之前先退订
      按键——iot_button_unregister_cb() 会清掉 single_click 上的全部回调（含
      首页那一份），而首页是在显示会话释放时才重新订阅的，顺序反了退出后
      首页按键就失灵。
--]]
local bm = require("board_manager")
local lvgl = require("lvgl")
local delay = require("delay")
local system_ok, system = pcall(require, "system")
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

local RUN_TIME_MS = int_arg("run_time_ms", 1800000)
local START_SPEED_MS = int_arg("start_speed_ms", 170)
local MIN_SPEED_MS = int_arg("min_speed_ms", 70)
local SPEED_STEP_MS = int_arg("speed_step_ms", 4)

-- 配色跟着整机走：黑底、近黑面板、金色强调，和 SquareLine 首页、
-- meeting_presenter 是同一套。
local C_BG        = "#000000"
local C_PANEL     = "#080C08"
local C_SUNKEN    = "#101820"
local C_ACCENT    = "#F0C428"
local C_ACCENT_HI = "#FFE45B"
local C_DIM       = "#384038"
local C_TEXT      = "#FAFBF5"
local C_TEXT_DIM  = "#788078"
local C_HEAD      = "#8CE64B"
local C_BODY      = "#3FA34D"
local C_BODY_ALT  = "#357F44"
local C_FOOD      = "#F65E3B"

local running = true
local key_group = nil
-- 实体键回调只置这个标志，真正的重开留在主循环里做
local restart_requested = false

local function quit()
    running = false
end

---------------------------------------------------------------- 初始化

-- 先要显示会话再开触摸：board_manager 的 lcd_touch 设备只有在显示起来之后
-- 才有 handle，顺序反了就只剩实体按键能用。
local ok, err = pcall(lvgl.init, {
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
    font_size = 18,
})
if not ok then
    print("[snake_game] ERROR: lvgl.init failed: " .. tostring(err))
    return
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C_BG, bg_opa = 255 })
scr:set_scroll({ dir = "none", scrollbar = "off" })

local scr_w, scr_h = scr:get_size()
if not scr_w or scr_w <= 0 or not scr_h or scr_h <= 0 then
    print("[snake_game] ERROR: invalid screen size")
    lvgl.deinit()
    return
end

-- 触摸手柄只是借给 LVGL 做 indev；owner 是 board_manager，deinit 不会释放它
local touch_handle = bm.get_lcd_touch_handle("lcd_touch")
if touch_handle then
    local registered, reg_err = pcall(lvgl.indev_register, "touch", touch_handle)
    if not registered then
        print("[snake_game] WARN: indev_register failed: " .. tostring(reg_err))
        touch_handle = nil
    end
else
    print("[snake_game] WARN: no lcd_touch handle, only the physical keys will work")
end

---------------------------------------------------------------- 布局

-- 顶栏 + 内容区。棋盘在左，方向键在右——800x480 横屏下比在底部挤一排按键
-- 舒展得多，也留出了放计分卡的地方。
local TOP_H = 56
local MARGIN = 20

local COLS, ROWS = 16, 16
local CELL_GAP = 2

local board_side = scr_h - TOP_H - MARGIN * 2
if board_side > math.floor(scr_w * 0.55) then
    board_side = math.floor(scr_w * 0.55)
end
local cell = math.floor((board_side - (COLS + 1) * CELL_GAP) / COLS)
local board_w = COLS * cell + (COLS + 1) * CELL_GAP
local board_h = ROWS * cell + (ROWS + 1) * CELL_GAP
local board_x = MARGIN
local board_y = TOP_H + math.floor((scr_h - TOP_H - board_h) / 2)

-- 棋盘左上角是容器内边距为 0 的坐标，格子 (col,row) 都是 1 起
local function cell_x(col)
    return CELL_GAP + (col - 1) * (cell + CELL_GAP)
end

local function cell_y(row)
    return CELL_GAP + (row - 1) * (cell + CELL_GAP)
end

-- set_pos() 要整数，任何除法结果都得落回整数
local SEG_SIZE = cell
local SEG_INSET = 1
local SEG_DRAW = cell - SEG_INSET * 2
local SEG_RADIUS = math.max(2, math.floor(cell / 5))
local FOOD_SIZE = cell - 4

---------------------------------------------------------------- 顶栏

local top_bar = lvgl.container(scr, { w = scr_w, h = TOP_H, x = 0, y = 0 })
top_bar:set_style({ bg_color = C_PANEL, bg_opa = 255, radius = 0, border_width = 0, pad = 0 })
top_bar:set_scroll({ dir = "none", scrollbar = "off" })

local top_tick = lvgl.container(top_bar, { w = 9, h = 34, align = "left_mid", x = 14 })
top_tick:set_style({ bg_color = C_ACCENT_HI, bg_opa = 255, radius = 2, border_width = 0 })

lvgl.label(top_bar, { text = "贪吃蛇  SNAKE", align = "left_mid", x = 40, text_color = C_TEXT })

-- 计分卡。set_style 没有 text_align，宽 label 里的字是左对齐的，所以用
-- align="center" 把自动收缩到内容大小的 label 摆到卡片正中——对象居中，
-- 视觉上就是文字居中。
local function make_card(parent, x_offset)
    local card = lvgl.container(parent, { w = 132, h = 38, align = "right_mid", x = x_offset })
    card:set_style({ bg_color = C_SUNKEN, bg_opa = 255, radius = 10, border_width = 0, pad = 0 })
    card:set_scroll({ dir = "none", scrollbar = "off" })
    -- 文字用底色直接画在卡片上，不走 label 的 align——label 的坐标要整数，
    -- 而且它不收缩到内容大小，宽 w 的 label 在 LVGL 里是靠左的
    -- （lvgl 模块本身没有居中文本的接口：set_style 没有 text_align，
    --   obj:align() 移动的是控件不是控件里的字。）
    local label = lvgl.label(card, {
        text = "0",
        w = 132,
        h = 38,
        x = 0,
        y = 0,
        text_color = C_ACCENT,
    })
    return label
end

local best_label = make_card(top_bar, -MARGIN)
local score_label = make_card(top_bar, -MARGIN - 132 - 12)

---------------------------------------------------------------- 棋盘

local board = lvgl.container(scr, { w = board_w, h = board_h, x = board_x, y = board_y })
board:set_style({
    bg_color = C_SUNKEN,
    bg_opa = 255,
    radius = 14,
    border_color = C_DIM,
    border_width = 3,
    pad = 0,
})
board:set_scroll({ dir = "none", scrollbar = "off" })

---------------------------------------------------------------- 右侧控制区

local rx = board_x + board_w + 24
local rw = scr_w - rx - MARGIN

-- 退出键压到整摞控件最底下，方向键那几行按剩余高度居中
local EXIT_H = 44
local HINT_H = 22
local EXIT_GAP = 26

local KEY_W = 92
local KEY_H = 60
local KEY_GAP = 8
if rw < KEY_W * 3 + KEY_GAP * 2 then
    KEY_W = math.max(56, math.floor((rw - KEY_GAP * 2) / 3))
    KEY_H = math.max(44, math.floor(KEY_W * 0.65))
end
local dpad_w = KEY_W * 3 + KEY_GAP * 2
local pad_x = rx + math.max(0, math.floor((rw - dpad_w) / 2))

local exit_y = scr_h - MARGIN - EXIT_H
local pad_area_bottom = exit_y - EXIT_GAP
local pad_h = KEY_H * 2 + KEY_GAP
local pad_y = TOP_H + math.max(MARGIN, math.floor((pad_area_bottom - TOP_H - pad_h) / 2))

local function make_key(text, x, y)
    local key = lvgl.button(scr, {
        text = text,
        x = x,
        y = y,
        w = KEY_W,
        h = KEY_H,
        bg_color = C_PANEL,
        text_color = C_TEXT,
        radius = 14,
        border_color = C_DIM,
        border_width = 4,
    })
    return key
end

local col_mid = pad_x + KEY_W + KEY_GAP
local row2_y = pad_y + KEY_H + KEY_GAP

local up_key = make_key("上", col_mid, pad_y)
local left_key = make_key("左", pad_x, row2_y)
local down_key = make_key("下", col_mid, row2_y)
local right_key = make_key("右", pad_x + (KEY_W + KEY_GAP) * 2, row2_y)

local exit_key = lvgl.button(scr, {
    text = "退出  MODE",
    x = pad_x,
    y = exit_y,
    w = dpad_w,
    h = EXIT_H,
    bg_color = C_PANEL,
    text_color = C_ACCENT,
    radius = 12,
    border_color = C_ACCENT,
    border_width = 3,
})

local hint_label = lvgl.label(scr, {
    text = "",
    x = pad_x,
    y = exit_y - HINT_H - 8,
    text_color = C_TEXT_DIM,
})

-- 按键按下时闪一下，按键和触摸给的反馈一致
local FLASH_TICKS = 12
local flashes = {}

local function flash(key)
    key:set_style({ bg_color = C_DIM, border_color = C_ACCENT_HI })
    flashes[key] = FLASH_TICKS
end

local function flash_tick()
    for key, ticks in pairs(flashes) do
        ticks = ticks - 1
        if ticks <= 0 then
            key:set_style({ bg_color = C_PANEL, border_color = C_DIM })
            flashes[key] = nil
        else
            flashes[key] = ticks
        end
    end
end

---------------------------------------------------------------- 游戏状态

local snake = {}            -- 1 是蛇头，元素是 {col=, row=}
local seg_pool = {}         -- 复用的格子对象，比每帧增删控件省得多
local food_obj = nil
local food = { col = 0, row = 0 }
local direction = { col = 0, row = -1 }
local next_direction = { col = 0, row = -1 }
local score = 0
local best = 0
local game_over = false
local step_ms = START_SPEED_MS
local next_step_at = 0
local overlay = nil

-- 转向队列：声明在 start_game / step 之前，两个函数都能正确引用这个局部变量
local pending_turns = {}
local PENDING_MAX = 4

math.randomseed((os.time() or 1) + scr_w * 17 + scr_h * 31 + now_ms())

local function seg_at(index)
    local seg = seg_pool[index]
    if seg and seg:is_valid() then
        return seg
    end
    seg = lvgl.container(board, { w = SEG_DRAW, h = SEG_DRAW })
    seg:set_style({ bg_color = C_BODY, bg_opa = 255, radius = SEG_RADIUS, border_width = 0, pad = 0 })
    seg:set_scroll({ dir = "none", scrollbar = "off" })
    seg_pool[index] = seg
    return seg
end

local function seg_hide(index)
    local seg = seg_pool[index]
    if seg and seg:is_valid() then
        seg:set_style({ opa = 0 })
    end
end

local function place_food()
    local occupied = {}
    for _, s in ipairs(snake) do
        occupied[s.col .. "," .. s.row] = true
    end

    local free_cells = {}
    for col = 1, COLS do
        for row = 1, ROWS do
            if not occupied[col .. "," .. row] then
                free_cells[#free_cells + 1] = { col = col, row = row }
            end
        end
    end
    if #free_cells == 0 then
        return
    end

    local pick = free_cells[math.random(1, #free_cells)]
    food.col = pick.col
    food.row = pick.row
    if not food_obj or not food_obj:is_valid() then
        food_obj = lvgl.container(board, { w = FOOD_SIZE, h = FOOD_SIZE })
        food_obj:set_style({ bg_color = C_FOOD, bg_opa = 255, radius = FOOD_SIZE, border_width = 0, pad = 0 })
        food_obj:set_scroll({ dir = "none", scrollbar = "off" })
    end
    food_obj:set_pos(cell_x(food.col) + 2, cell_y(food.row) + 2)
end

local function draw_snake()
    for i, s in ipairs(snake) do
        local seg = seg_at(i)
        seg:set_pos(cell_x(s.col) + SEG_INSET, cell_y(s.row) + SEG_INSET)
        if i == 1 then
            seg:set_style({ bg_color = C_HEAD, opa = 255, radius = SEG_RADIUS })
        elseif i % 2 == 0 then
            seg:set_style({ bg_color = C_BODY, opa = 255, radius = SEG_RADIUS })
        else
            seg:set_style({ bg_color = C_BODY_ALT, opa = 255, radius = SEG_RADIUS })
        end
    end
    for i = #snake + 1, #seg_pool do
        seg_hide(i)
    end
end

local function update_hud()
    score_label:set_text("得分 " .. score)
    best_label:set_text("最高 " .. best)
    if game_over then
        hint_label:set_text("MODE 退出")
    elseif key_group then
        hint_label:set_text("VOL+ / VOL- 上下")
    else
        hint_label:set_text("")
    end
end

local function hide_overlay()
    if overlay and overlay:is_valid() then
        overlay:delete()
    end
    overlay = nil
end

local function show_overlay()
    hide_overlay()
    overlay = lvgl.container(board, {
        w = board_w - 60,
        h = 150,
        align = "center",
        bg_color = C_PANEL,
        bg_opa = 255,
        radius = 16,
        border_color = C_FOOD,
        border_width = 6,
        pad = 0,
    })
    overlay:set_scroll({ dir = "none", scrollbar = "off" })

    lvgl.label(overlay, { text = "游戏结束", align = "center", y = -40, text_color = C_FOOD })
    lvgl.label(overlay, { text = "得分 " .. score, align = "center", y = -6, text_color = C_TEXT })
    lvgl.label(overlay, { text = "方向键或触摸屏幕重开", align = "center", y = 26, text_color = C_TEXT_DIM })
end

local function start_game()
    hide_overlay()
    snake = {}
    pending_turns = {}
    score = 0
    game_over = false
    step_ms = START_SPEED_MS
    direction = { col = 0, row = -1 }
    next_direction = { col = 0, row = -1 }
    restart_requested = false

    local start_col = math.floor(COLS / 2)
    local start_row = math.floor(ROWS / 2)
    for i = 1, 4 do
        snake[i] = { col = start_col, row = start_row + i - 1 }
    end

    place_food()
    draw_snake()
    update_hud()
    next_step_at = now_ms() + step_ms
end

---------------------------------------------------------------- 一步

local function apply_turns()
    -- 参照系是这一帧真正走的方向，不是队列里上一条命令——否则「上→左」
    -- 会被判成反向而被丢掉
    local last_col = direction.col
    local last_row = direction.row

    for _, turn in ipairs(pending_turns) do
        if turn.col == -last_col and turn.row == -last_row then
            -- 反向，丢弃
        elseif turn.col == last_col and turn.row == last_row then
            -- 和当前方向相同，丢弃
        elseif turn.col == 0 and turn.row == 0 then
            -- 丢弃
        else
            next_direction = turn
            last_col = turn.col
            last_row = turn.row
        end
    end
    pending_turns = {}
end

local function step()
    apply_turns()
    direction = next_direction

    local head = snake[1]
    local col = head.col + direction.col
    local row = head.row + direction.row

    -- 穿墙
    if col < 1 then col = COLS end
    if col > COLS then col = 1 end
    if row < 1 then row = ROWS end
    if row > ROWS then row = 1 end

    -- 撞自己。最后一节这一步会让位，所以不参与判定。
    for i = 1, #snake - 1 do
        if snake[i].col == col and snake[i].row == row then
            game_over = true
            if score > best then best = score end
            show_overlay()
            update_hud()
            print("[snake_game] game over, score=" .. score)
            return
        end
    end

    table.insert(snake, 1, { col = col, row = row })

    if col == food.col and row == food.row then
        score = score + 10
        if score > best then best = score end
        step_ms = math.max(MIN_SPEED_MS, step_ms - SPEED_STEP_MS)
        place_food()
    else
        table.remove(snake)
    end

    draw_snake()
    if not game_over then
        update_hud()
    end
end

---------------------------------------------------------------- 输入

-- 主循环里按到达顺序逐条处理，同一次迭代里连续两次转向也都能生效
-- （直接写 next_direction 的话第一次会被第二次覆盖，转身就丢了）

local function push_turn(dcol, drow)
    if #pending_turns >= PENDING_MAX then
        return
    end
    pending_turns[#pending_turns + 1] = { col = dcol, row = drow }
end

local function turn_up()    push_turn(0, -1) end
local function turn_down()  push_turn(0, 1) end
local function turn_left()  push_turn(-1, 0) end
local function turn_right() push_turn(1, 0) end

-- 方向键：活着转向，结束了就重开
local function steer(key, turn)
    return function()
        flash(key)
        if game_over then
            start_game()
            return
        end
        turn()
    end
end

up_key:on("clicked", steer(up_key, turn_up))
down_key:on("clicked", steer(down_key, turn_down))
left_key:on("clicked", steer(left_key, turn_left))
right_key:on("clicked", steer(right_key, turn_right))
exit_key:on("clicked", quit)

---------------------------------------------------------------- 实体按键

-- 键位靠 YAML button_labels 认，认不出来时按硬件顺序兜底。
-- 四键板子只有 VOLUME_UP / VOLUME_DOWN / MODE / SET，所以左右在这里走不到。
local KEY_ORDER = { "VOLUME_UP", "VOLUME_DOWN", "MODE", "SET" }
local KEY_ACTIONS = {
    VOLUME_UP   = "up",
    VOLUME_DOWN = "down",
    MODE        = "quit",
    SET         = "restart",
}

-- 回调跑在按钮的事件任务里，只置标志，真正的重开留给主循环，
-- 免得在回调里重建整盘蛇
local function init_keys()
    if not button_ok or not button then
        print("[snake_game] WARN: require(button) failed, MODE exit unavailable")
        return
    end

    local opened, group = pcall(button.open_group, "adc_button_group")
    if not opened or not group then
        print("[snake_game] WARN: open_group(adc_button_group) failed: " .. tostring(group))
        return
    end
    key_group = group

    local labels = button.group_labels(group) or {}
    local by_label = {}
    for i, name in ipairs(labels) do
        by_label[name] = KEY_ACTIONS[name] or KEY_ACTIONS[KEY_ORDER[i]]
    end

    -- single_click 而不是 press_down：一步 170ms，按住不放连发反而会自撞
    local subscribed, err = pcall(button.on, group, "single_click", function(evt)
        local action = evt.label and by_label[evt.label]
        if action == nil then
            return
        end
        if action == "quit" then
            quit()
        elseif game_over then
            restart_requested = true
        elseif action == "up" then
            turn_up()
        elseif action == "down" then
            turn_down()
        end
    end)
    if not subscribed then
        print("[snake_game] WARN: button.on failed: " .. tostring(err))
        pcall(button.close, group)
        key_group = nil
    end
end

init_keys()

---------------------------------------------------------------- 主循环

start_game()
scr:load()
print(string.format("[snake_game] ready screen=%dx%d grid=%dx%d cell=%d run_ms=%d",
    scr_w, scr_h, COLS, ROWS, cell, RUN_TIME_MS))
if game_over then
    print("[snake_game] WARN: game over during init")
end

local FRAME_MS = 10
local start = now_ms()

while running and now_ms() - start < RUN_TIME_MS do
    if key_group then
        button.dispatch()
        if not running then
            break
        end
        if restart_requested then
            restart_requested = false
            start_game()
        end
    end

    lvgl.process_events(FRAME_MS)
    flash_tick()

    local now = now_ms()
    if not game_over and now >= next_step_at then
        step()
        next_step_at = now + step_ms
    end

    delay.delay_ms(FRAME_MS)
end

---------------------------------------------------------------- 收尾

-- 顺序要紧：先退订按键再关显示会话，理由见文件头
if key_group then
    pcall(button.off, key_group)
    pcall(button.close, key_group)
    key_group = nil
end
hide_overlay()
if touch_handle then
    pcall(lvgl.indev_unregister, "touch")
end
lvgl.deinit()
print("[snake_game] done, score=" .. score)

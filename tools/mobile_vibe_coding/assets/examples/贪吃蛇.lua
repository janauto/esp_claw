local board_manager = require("board_manager")
local lvgl = require("lvgl")
local lcd_touch = require("lcd_touch")
local delay = require("delay")

-- 初始化板级外设
board_manager.init_device("display_lcd")
local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")

-- 初始化 LVGL
lvgl.init({
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
    font_size = 18,
})

-- 注册触摸
if touch_handle then
    lvgl.indev_register("touch", touch_handle)
    print("触摸已注册")
end

-- 获取屏幕尺寸
local scr = lvgl.create_screen()
local scr_w, scr_h = scr:get_size()
print("屏幕尺寸: " .. scr_w .. "x" .. scr_h)

-- 游戏常量
local CELL_SIZE = 20
local GAME_AREA_H = 370
local BTN_AREA_Y = 375
local BTN_AREA_H = 105

local GRID_COLS = math.floor(scr_w / CELL_SIZE)
local GRID_ROWS = math.floor(GAME_AREA_H / CELL_SIZE)

print("游戏网格: " .. GRID_COLS .. "x" .. GRID_ROWS .. " 格子尺寸: " .. CELL_SIZE)

-- 游戏状态
local snake_segments = {}
local snake_positions = {}
local food_obj = nil
local food_pos = {x = 0, y = 0}
local direction = {x = 0, y = -1}
local next_direction = {x = 0, y = -1}
local score = 0
local game_over = false
local game_speed = 150
local frame_count = 0
local ticks_per_move = math.floor(game_speed / 10)

-- 游戏区域背景
local game_bg = lvgl.object(scr, {
    w = scr_w,
    h = GAME_AREA_H,
    x = 0,
    y = 0,
    bg_color = "#101820",
})

-- 创建蛇段对象
local function create_segment(x, y, is_head)
    local color = is_head and "#00ff00" or "#00cc00"
    local seg = lvgl.object(scr, {
        x = x * CELL_SIZE + 1,
        y = y * CELL_SIZE + 1,
        w = CELL_SIZE - 2,
        h = CELL_SIZE - 2,
        bg_color = color,
        radius = 2,
    })
    return seg
end

-- 创建食物对象
local function create_food(x, y)
    if food_obj and food_obj:is_valid() then
        food_obj:delete()
    end
    food_obj = lvgl.object(scr, {
        x = x * CELL_SIZE + 2,
        y = y * CELL_SIZE + 2,
        w = CELL_SIZE - 4,
        h = CELL_SIZE - 4,
        bg_color = "#ff3333",
        radius = 4,
    })
end

-- 初始化蛇
local function init_snake()
    for _, seg in ipairs(snake_segments) do
        if seg:is_valid() then
            seg:delete()
        end
    end
    snake_segments = {}
    snake_positions = {}

    local start_x = math.floor(GRID_COLS / 2)
    local start_y = math.floor(GRID_ROWS / 2)

    for i = 1, 4 do
        local x = start_x
        local y = start_y + i - 1
        snake_positions[i] = {x = x, y = y}
        snake_segments[i] = create_segment(x, y, i == 1)
    end

    direction = {x = 0, y = -1}
    next_direction = {x = 0, y = -1}
    score = 0
    game_over = false
    game_speed = 150
    frame_count = 0
    ticks_per_move = math.floor(game_speed / 10)
end

-- 生成食物
local function spawn_food()
    local occupied = {}
    for _, pos in ipairs(snake_positions) do
        occupied[pos.x .. "," .. pos.y] = true
    end

    local free_cells = {}
    for x = 0, GRID_COLS - 1 do
        for y = 0, GRID_ROWS - 1 do
            if not occupied[x .. "," .. y] then
                free_cells[#free_cells + 1] = {x = x, y = y}
            end
        end
    end

    if #free_cells > 0 then
        local idx = math.random(1, #free_cells)
        food_pos = free_cells[idx]
        create_food(food_pos.x, food_pos.y)
    end
end

-- 更新蛇段视觉位置
local function update_snake_visuals()
    for i, pos in ipairs(snake_positions) do
        if snake_segments[i] and snake_segments[i]:is_valid() then
            snake_segments[i]:set_pos(pos.x * CELL_SIZE + 1, pos.y * CELL_SIZE + 1)
        end
    end
end

-- 游戏逻辑
local function game_tick()
    if game_over then
        return
    end

    direction = {x = next_direction.x, y = next_direction.y}

    local head = snake_positions[1]
    local new_x = head.x + direction.x
    local new_y = head.y + direction.y

    -- 穿墙：从对面墙壁出来
    if new_x < 0 then new_x = GRID_COLS - 1 end
    if new_x >= GRID_COLS then new_x = 0 end
    if new_y < 0 then new_y = GRID_ROWS - 1 end
    if new_y >= GRID_ROWS then new_y = 0 end

    -- 检查是否撞到自己
    for i, pos in ipairs(snake_positions) do
        if pos.x == new_x and pos.y == new_y then
            game_over = true
            print("游戏结束! 撞到自己了! 得分: " .. score)
            return
        end
    end

    -- 移动蛇头
    table.insert(snake_positions, 1, {x = new_x, y = new_y})

    -- 检查是否吃到食物
    if new_x == food_pos.x and new_y == food_pos.y then
        score = score + 10
        print("吃到食物! 得分: " .. score)
        local tail = snake_positions[#snake_positions]
        snake_segments[#snake_segments + 1] = create_segment(tail.x, tail.y, false)
        spawn_food()
        if game_speed > 60 then
            game_speed = game_speed - 2
            ticks_per_move = math.floor(game_speed / 10)
        end
    else
        table.remove(snake_positions)
    end

    update_snake_visuals()
end

-- 重新开始游戏
local function restart_game()
    init_snake()
    spawn_food()
    update_snake_visuals()
    print("游戏重新开始")
end

-- 得分标签（放在游戏区域顶部）
local score_label = lvgl.label(scr, {
    text = "得分: 0",
    x = 5,
    y = 5,
    text_color = "#ffffff",
})

-- 按钮区域背景
local btn_bg = lvgl.object(scr, {
    w = scr_w,
    h = BTN_AREA_H,
    x = 0,
    y = BTN_AREA_Y,
    bg_color = "#1a1a2e",
})

-- 按钮尺寸与位置计算
local BTN_W = 70
local BTN_H = 42
local center_x = math.floor(scr_w / 2)

local up_btn_x = center_x - math.floor(BTN_W / 2)
local up_btn_y = BTN_AREA_Y + 5
local row2_y = BTN_AREA_Y + 5 + BTN_H + 5
local left_btn_x = center_x - BTN_W - 10 - math.floor(BTN_W / 2)
local down_btn_x = center_x - math.floor(BTN_W / 2)
local right_btn_x = center_x + 10 + math.floor(BTN_W / 2)

-- 创建方向按钮
local up_btn = lvgl.button(scr, {
    text = "上",
    x = up_btn_x,
    y = up_btn_y,
    w = BTN_W,
    h = BTN_H,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

local down_btn = lvgl.button(scr, {
    text = "下",
    x = down_btn_x,
    y = row2_y,
    w = BTN_W,
    h = BTN_H,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

local left_btn = lvgl.button(scr, {
    text = "左",
    x = left_btn_x,
    y = row2_y,
    w = BTN_W,
    h = BTN_H,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

local right_btn = lvgl.button(scr, {
    text = "右",
    x = right_btn_x,
    y = row2_y,
    w = BTN_W,
    h = BTN_H,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

-- 按钮回调：游戏结束时任意按钮可重新开始
up_btn:on("clicked", function()
    if game_over then
        restart_game()
        return
    end
    if direction.y ~= 1 then
        next_direction = {x = 0, y = -1}
        print("方向: 上")
    end
end)

down_btn:on("clicked", function()
    if game_over then
        restart_game()
        return
    end
    if direction.y ~= -1 then
        next_direction = {x = 0, y = 1}
        print("方向: 下")
    end
end)

left_btn:on("clicked", function()
    if game_over then
        restart_game()
        return
    end
    if direction.x ~= 1 then
        next_direction = {x = -1, y = 0}
        print("方向: 左")
    end
end)

right_btn:on("clicked", function()
    if game_over then
        restart_game()
        return
    end
    if direction.x ~= -1 then
        next_direction = {x = 1, y = 0}
        print("方向: 右")
    end
end)

-- 更新得分显示
local function update_score_display()
    local status_text = "得分: " .. score
    if game_over then
        status_text = status_text .. "  游戏结束! 点击按钮重新开始"
    end
    score_label:set_text(status_text)
end

-- 初始化游戏
init_snake()
spawn_food()
update_snake_visuals()
update_score_display()
print("贪吃蛇游戏开始! 网格: " .. GRID_COLS .. "x" .. GRID_ROWS .. " | 穿墙模式 | 按钮: 上/下/左/右")

scr:load()

-- 主循环
while true do
    -- 同步触摸
    if touch_handle then
        lcd_touch.sync(touch_handle)
    end

    -- 处理 LVGL 事件
    lvgl.process_events(10)

    -- 游戏时钟
    frame_count = frame_count + 1
    if frame_count >= ticks_per_move then
        frame_count = 0
        game_tick()
        update_score_display()
    end

    -- 游戏结束时触摸屏幕任意位置也可以重新开始
    if game_over and touch_handle then
        local touch = lcd_touch.poll(touch_handle)
        if touch and touch.just_pressed then
            restart_game()
            update_score_display()
        end
    end

    delay.delay_ms(10)
end

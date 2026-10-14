local lvgl = require("lvgl")
local board_manager = require("board_manager")
local delay = require("delay")
local storage = require("storage")

-- 初始化 LVGL
lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

-- 注册触摸设备
local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")
if touch_handle then
    lvgl.indev_register("touch", touch_handle)
end

-- 获取屏幕尺寸
local panel_handle, io_handle, screen_w, screen_h, panel_if =
    board_manager.get_display_lcd_params("display_lcd")
screen_w = screen_w or 320
screen_h = screen_h or 240

-- 创建屏幕
local scr = lvgl.create_screen()
scr:set_style({ bg_color = 0x87CEEB, bg_opa = 255 })
scr:load()

-- 糖果颜色表（6种）
local candy_colors = {
    0xFF0000, -- 红
    0x00FF00, -- 绿
    0x0000FF, -- 蓝
    0xFFFF00, -- 黄
    0xFF00FF, -- 紫
    0x00FFFF, -- 青
}
local empty_color = 0x87CEEB -- 与背景相同

-- 网格参数
local GRID_SIZE = 8
local CELL_SIZE = 28
local PADDING = 4
local total_w = GRID_SIZE * CELL_SIZE + (GRID_SIZE - 1) * PADDING
local total_h = total_w
local start_x = math.floor((screen_w - total_w) / 2)
local start_y = math.floor((screen_h - total_h) / 2) - 10

-- 游戏状态
local grid_buttons = {}
local grid_colors = {}
local score = 0
local high_score = 0
local selected_row = nil
local selected_col = nil
local processing = false

-- 创建得分和最高分标签
local score_label = lvgl.label(scr, {
    text = "分数: 0",
    align = "top_mid",
    y = start_y + total_h + 10,
    text_color = 0x000000,
})
local high_score_label = lvgl.label(scr, {
    text = "最高分: 0",
    align = "top_mid",
    y = start_y + total_h + 35,
    text_color = 0x000000,
})

-- 读取最高分记录
local high_score_file = storage.join_path(storage.get_root_dir(), "highscore.txt")
if storage.exists(high_score_file) then
    local content = storage.read_file(high_score_file)
    if content then
        high_score = tonumber(content) or 0
    end
end
high_score_label:set_text("最高分: " .. high_score)

-- 辅助函数
local function set_cell_color(row, col, color)
    local btn = grid_buttons[row][col]
    if btn and btn:is_valid() then
        btn:set_style({ bg_color = color })
    end
end

local function get_cell_color(row, col)
    return grid_colors[row][col]
end

local function set_cell_empty(row, col)
    grid_colors[row][col] = 0
    set_cell_color(row, col, empty_color)
end

local function is_cell_empty(row, col)
    return grid_colors[row][col] == 0
end

local function random_color()
    local idx = math.random(1, #candy_colors)
    return candy_colors[idx]
end

local function has_match()
    -- 水平检查
    for r = 1, GRID_SIZE do
        for c = 1, GRID_SIZE - 2 do
            local color = grid_colors[r][c]
            if color ~= 0 and color == grid_colors[r][c+1] and color == grid_colors[r][c+2] then
                return true
            end
        end
    end
    -- 垂直检查
    for c = 1, GRID_SIZE do
        for r = 1, GRID_SIZE - 2 do
            local color = grid_colors[r][c]
            if color ~= 0 and color == grid_colors[r+1][c] and color == grid_colors[r+2][c] then
                return true
            end
        end
    end
    return false
end

-- 初始化网格，确保无初始三消
local function init_grid()
    math.randomseed(0) -- 固定种子，保证可重复性
    for row = 1, GRID_SIZE do
        grid_colors[row] = {}
        for col = 1, GRID_SIZE do
            grid_colors[row][col] = random_color()
        end
    end
    -- 反复随机直到无消除
    while has_match() do
        for row = 1, GRID_SIZE do
            for col = 1, GRID_SIZE do
                grid_colors[row][col] = random_color()
            end
        end
    end
    -- 创建按钮
    for row = 1, GRID_SIZE do
        grid_buttons[row] = {}
        for col = 1, GRID_SIZE do
            local x = start_x + (col - 1) * (CELL_SIZE + PADDING)
            local y = start_y + (row - 1) * (CELL_SIZE + PADDING)
            local btn = lvgl.button(scr, {
                w = CELL_SIZE,
                h = CELL_SIZE,
                x = x,
                y = y,
                bg_color = grid_colors[row][col],
                radius = 5,
                border_width = 0,
            })
            grid_buttons[row][col] = btn
            -- 注册点击事件（闭包捕获行列）
            btn:on("clicked", function()
                if processing then return end
                if is_cell_empty(row, col) then return end
                if selected_row == nil then
                    -- 选中当前糖果
                    selected_row = row
                    selected_col = col
                    btn:set_style({ border_width = 3, border_color = 0xFFFFFF })
                else
                    local sr, sc = selected_row, selected_col
                    -- 取消之前的高亮
                    local sel_btn = grid_buttons[sr][sc]
                    if sel_btn and sel_btn:is_valid() then
                        sel_btn:set_style({ border_width = 0 })
                    end
                    selected_row = nil
                    selected_col = nil
                    -- 检查是否相邻
                    local dr = math.abs(row - sr)
                    local dc = math.abs(col - sc)
                    if (dr == 1 and dc == 0) or (dr == 0 and dc == 1) then
                        -- 交换
                        processing = true
                        local color1 = grid_colors[sr][sc]
                        local color2 = grid_colors[row][col]
                        grid_colors[sr][sc] = color2
                        grid_colors[row][col] = color1
                        set_cell_color(sr, sc, color2)
                        set_cell_color(row, col, color1)
                        lvgl.process_events(0)
                        delay.delay_ms(200)
                        -- 检查是否形成消除
                        if not has_match() then
                            -- 无效交换，换回来
                            grid_colors[sr][sc] = color1
                            grid_colors[row][col] = color2
                            set_cell_color(sr, sc, color1)
                            set_cell_color(row, col, color2)
                            lvgl.process_events(0)
                            delay.delay_ms(200)
                            processing = false
                            return
                        end
                        -- 消除循环
                        while has_match() do
                            -- 标记要消除的格子
                            local to_eliminate = {}
                            for r = 1, GRID_SIZE do
                                to_eliminate[r] = {}
                                for c = 1, GRID_SIZE do
                                    to_eliminate[r][c] = false
                                end
                            end
                            -- 水平标记
                            for r = 1, GRID_SIZE do
                                local c = 1
                                while c <= GRID_SIZE - 2 do
                                    local color = grid_colors[r][c]
                                    if color ~= 0 and color == grid_colors[r][c+1] and color == grid_colors[r][c+2] then
                                        local start_c = c
                                        c = c + 3
                                        while c <= GRID_SIZE and grid_colors[r][c] == color do
                                            c = c + 1
                                        end
                                        for cc = start_c, c - 1 do
                                            to_eliminate[r][cc] = true
                                        end
                                    else
                                        c = c + 1
                                    end
                                end
                            end
                            -- 垂直标记
                            for c = 1, GRID_SIZE do
                                local r = 1
                                while r <= GRID_SIZE - 2 do
                                    local color = grid_colors[r][c]
                                    if color ~= 0 and color == grid_colors[r+1][c] and color == grid_colors[r+2][c] then
                                        local start_r = r
                                        r = r + 3
                                        while r <= GRID_SIZE and grid_colors[r][c] == color do
                                            r = r + 1
                                        end
                                        for rr = start_r, r - 1 do
                                            to_eliminate[rr][c] = true
                                        end
                                    else
                                        r = r + 1
                                    end
                                end
                            end
                            -- 消除
                            local eliminated_count = 0
                            for r = 1, GRID_SIZE do
                                for c = 1, GRID_SIZE do
                                    if to_eliminate[r][c] then
                                        set_cell_empty(r, c)
                                        eliminated_count = eliminated_count + 1
                                    end
                                end
                            end
                            score = score + eliminated_count * 10
                            score_label:set_text("分数: " .. score)
                            if score > high_score then
                                high_score = score
                                high_score_label:set_text("最高分: " .. high_score)
                                storage.write_file(high_score_file, tostring(high_score))
                            end
                            lvgl.process_events(0)
                            delay.delay_ms(200)
                            -- 下落填充
                            for cc = 1, GRID_SIZE do
                                local empty_row = GRID_SIZE
                                for rr = GRID_SIZE, 1, -1 do
                                    if not is_cell_empty(rr, cc) then
                                        if rr ~= empty_row then
                                            grid_colors[empty_row][cc] = grid_colors[rr][cc]
                                            set_cell_color(empty_row, cc, grid_colors[rr][cc])
                                            set_cell_empty(rr, cc)
                                            empty_row = empty_row - 1
                                        else
                                            empty_row = empty_row - 1
                                        end
                                    end
                                end
                                -- 填充顶部空位
                                for rr = empty_row, 1, -1 do
                                    local new_color = random_color()
                                    grid_colors[rr][cc] = new_color
                                    set_cell_color(rr, cc, new_color)
                                end
                            end
                            lvgl.process_events(0)
                            delay.delay_ms(200)
                        end
                        processing = false
                    else
                        -- 不相邻，切换选中
                        selected_row = row
                        selected_col = col
                        btn:set_style({ border_width = 3, border_color = 0xFFFFFF })
                    end
                end
            end)
        end
    end
end

-- 运行游戏
init_grid()

-- 主循环
while true do
    lvgl.process_events(16)
end

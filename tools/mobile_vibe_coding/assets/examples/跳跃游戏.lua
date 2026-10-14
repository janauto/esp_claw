-- 跳跃小游戏 v2 (Jump Game)
-- 点击屏幕让人物跳跃，躲避障碍物
-- 随着分数增加，速度会越来越快！
local lvgl = require("lvgl")
local board_manager = require("board_manager")
local delay = require("delay")
-- 游戏配置
local GRAVITY = 0.6         -- 重力
local JUMP_FORCE = -11      -- 跳跃力度
local GROUND_Y_RATIO = 0.8  -- 地面占屏幕高度的比例
local GRASS_HEIGHT = 15     -- 草地高度
local PLAYER_SIZE = 36      -- 玩家大小
local OBSTACLE_WIDTH = 25   -- 障碍物基础宽度
local OBSTACLE_MIN_HEIGHT = 20 -- 障碍物最小高度
local OBSTACLE_MAX_HEIGHT = 70 -- 障碍物最大高度
local BASE_OBSTACLE_SPEED = 4 -- 基础障碍物速度
local BASE_SPAWN_INTERVAL = 1800 -- 基础障碍物生成间隔（毫秒）
local MIN_SPAWN_INTERVAL = 800   -- 最小生成间隔
-- 获取毫秒时间戳
local function get_ms()
    -- os.time() 返回秒，乘以 1000 转为毫秒
    return math.floor(os.time() * 1000)
end
-- 游戏状态
local game_state = {
    player_y = 0,
    player_vy = 0,
    is_jumping = false,
    score = 0,
    is_running = false,
    is_game_over = false,
    obstacles = {},
    last_spawn = 0,
    speed_multiplier = 1.0,
}
-- UI 元素
local player_obj = nil
local score_label = nil
local game_over_label = nil
local start_label = nil
local scr = nil
local ground_y = 0
local baseline_y = 0  -- 游戏基准线（草地顶部）
local screen_w = 320
local screen_h = 240
local clouds = {}
-- 获取屏幕尺寸
local function get_screen_size()
    local panel_handle, io_handle, width, height, panel_if =
        board_manager.get_display_lcd_params("display_lcd")
    return width or 320, height or 240
end
-- 创建带样式的对象
local function create_obj(parent, w, h, x, y, color, radius, opa)
    local obj = lvgl.object(parent, {
        w = w,
        h = h,
        x = x,
        y = y,
    })
    obj:set_style({
        bg_color = color,
        bg_opa = opa or 255,
        radius = radius or 0,
        pad_all = 0,
        margin_all = 0,
    })
    return obj
end
-- 创建玩家（更可爱的小人）
local function create_player(parent)
    -- 身体 - 站在基准线上
    player_obj = lvgl.object(parent, {
        w = PLAYER_SIZE,
        h = PLAYER_SIZE,
        x = 60,
        y = baseline_y - PLAYER_SIZE,
    })
    player_obj:set_style({
        bg_color = 0x4CAF50,
        bg_opa = 255,
        radius = 8,
        pad_all = 0,
        margin_all = 0,
    })
    -- 眼睛
    local eye1 = create_obj(player_obj, 8, 8, 22, 8, 0xFFFFFF, 4)
    local eye2 = create_obj(player_obj, 8, 8, 6, 8, 0xFFFFFF, 4)
    local pupil1 = create_obj(player_obj, 4, 4, 24, 10, 0x333333, 2)
    local pupil2 = create_obj(player_obj, 4, 4, 8, 10, 0x333333, 2)
    -- 嘴巴
    local mouth = create_obj(player_obj, 16, 4, 10, 24, 0x333333, 2)
end
-- 创建障碍物（站在草地上）
local function create_obstacle(parent, x)
    local types = {"cactus", "rock", "box"}
    local obs_type = types[math.random(#types)]
    local width = OBSTACLE_WIDTH + math.random(15)
    local height = OBSTACLE_MIN_HEIGHT + math.random(OBSTACLE_MAX_HEIGHT - OBSTACLE_MIN_HEIGHT)
    local color
    if obs_type == "cactus" then
        color = 0x2E7D32
    elseif obs_type == "rock" then
        color = 0x757575
        width = width + 10
        height = height - 10
    else
        color = 0x8D6E63
    end
    -- 确保高度在合理范围内
    height = math.max(OBSTACLE_MIN_HEIGHT, math.min(height, OBSTACLE_MAX_HEIGHT))
    -- 障碍物底部对齐基准线
    local obs_y = baseline_y - height
    print(string.format("创建障碍物: baseline_y=%d, height=%d, obs_y=%d, 底部=%d",
        baseline_y, height, obs_y, obs_y + height))
    local obs = create_obj(parent, width, height, x, obs_y, color, 4)
    return obs, height, width, obs_y
end
-- 创建云朵装饰
local function create_clouds(parent)
    for i = 1, 3 do
        local cx = math.random(screen_w - 80)
        local cy = 20 + math.random(60)
        local cloud = create_obj(parent, 50 + math.random(30), 20 + math.random(15), cx, cy, 0xFFFFFF, 10, 180)
        table.insert(clouds, {
            obj = cloud,
            x = cx,
            speed = 0.5 + math.random(10) / 10,
        })
    end
end
-- 碰撞检测
local function check_collision(obs_obj, obs_x, obs_y, obs_w, obs_h)
    local px, py = player_obj:get_pos()
    -- 稍微缩小碰撞区域，让游戏更公平
    local margin = 6
    if px + PLAYER_SIZE - margin > obs_x + margin and px + margin < obs_x + obs_w - margin then
        if py + PLAYER_SIZE - margin > obs_y + margin and py + margin < obs_y + obs_h - margin then
            return true
        end
    end
    return false
end
-- 跳跃
local function jump()
    if not game_state.is_jumping and game_state.is_running and not game_state.is_game_over then
        game_state.player_vy = JUMP_FORCE
        game_state.is_jumping = true
    end
end
-- 重新开始游戏
local function restart_game()
    -- 清除旧障碍物
    for _, obs in ipairs(game_state.obstacles) do
        obs.obj:delete()
    end
    game_state.obstacles = {}
    -- 重置状态
    game_state.player_y = baseline_y
    game_state.player_vy = 0
    game_state.is_jumping = false
    game_state.score = 0
    game_state.is_running = true
    game_state.is_game_over = false
    game_state.last_spawn = get_ms()
    game_state.speed_multiplier = 1.0
    -- 更新 UI
    player_obj:set_pos(60, baseline_y - PLAYER_SIZE)
    score_label:set_text("分数: 0")
    game_over_label:set_text("")
    start_label:set_text("")
end
-- 开始游戏
local function start_game()
    game_state.is_running = true
    game_state.last_spawn = get_ms()
    start_label:set_text("")
end
-- 显示游戏结束
local function show_game_over()
    game_state.is_game_over = true
    game_state.is_running = false

    game_over_label:set_text("游戏结束!\n分数: " .. game_state.score .. "\n点击重新开始")
    game_over_label:set_style({
        text_color = 0xFFFFFF,
        bg_color = 0xE53935,
        bg_opa = 240,
        pad_all = 15,
        radius = 12,
    })
end
-- 安全设置位置（确保整数）
local function set_pos_int(obj, x, y)
    obj:set_pos(math.floor(x), math.floor(y))
end
-- 主函数
local function main()
    print("初始化跳跃小游戏...")
    lvgl.init({
        buffer_lines = 40,
        tick_ms = 5,
        task_period_ms = 10,
    })

    -- 注册触摸输入
    local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")
    if touch_handle then
        lvgl.indev_register("touch", touch_handle)
    end

    screen_w, screen_h = get_screen_size()
    ground_y = math.floor(screen_h * GROUND_Y_RATIO)
    baseline_y = ground_y - GRASS_HEIGHT  -- 基准线在草地顶部，对象站在草地表面
    game_state.player_y = baseline_y
    print("屏幕尺寸: " .. screen_w .. "x" .. screen_h)
    print("地面Y坐标: " .. ground_y)
    print("基准线Y坐标: " .. baseline_y)

    -- 创建主屏幕
    scr = lvgl.create_screen()
    scr:set_style({ bg_color = 0x87CEEB, bg_opa = 255 }) -- 天空蓝背景
    scr:load()

    -- 创建太阳
    create_obj(scr, 40, 40, screen_w - 60, 20, 0xFFD54F, 20, 255)

    -- 创建云朵
    create_clouds(scr)
    -- 创建地面（棕色土地）
    create_obj(scr, screen_w, screen_h - ground_y, 0, ground_y, 0x8B4513, 0, 255)
    -- 创建草地（在地面上方，向上延伸）
    create_obj(scr, screen_w, GRASS_HEIGHT, 0, ground_y - GRASS_HEIGHT, 0x4CAF50, 0, 255)
    -- 创建草地装饰
    for i = 0, screen_w, 20 do
        create_obj(scr, 4, 8, i + math.random(5), ground_y - GRASS_HEIGHT - 3, 0x388E3C, 2, 255)
    end

    -- 创建玩家
    create_player(scr)

    -- 分数显示
    score_label = lvgl.label(scr, {
        text = "分数: 0",
        align = "top_mid",
        y = 15,
    })
    score_label:set_style({
        bg_color = 0x333333,
        bg_opa = 200,
        radius = 8,
        pad_all = 8,
        text_color = 0xFFFFFF,
    })

    -- 开始提示
    start_label = lvgl.label(scr, {
        text = "点击屏幕开始游戏!",
        align = "center",
        y = -60,
    })
    start_label:set_style({
        text_color = 0xFFFFFF,
        bg_color = 0x333333,
        bg_opa = 200,
        pad_all = 12,
        radius = 10,
    })

    -- 游戏结束提示
    game_over_label = lvgl.label(scr, {
        text = "",
        align = "center",
        y = -60,
    })
    game_over_label:set_style({
        text_color = 0xFFFFFF,
        bg_color = 0xE53935,
        bg_opa = 240,
        pad_all = 15,
        radius = 12,
    })

    -- 点击事件
    scr:on("clicked", function()
        print("屏幕被点击")
        if game_state.is_game_over then
            print("重新开始游戏")
            restart_game()
        elseif not game_state.is_running then
            print("开始游戏")
            start_game()
        else
            print("跳跃!")
            jump()
        end
    end)

    -- 游戏主循环
    local last_cloud_update = get_ms()
    local last_frame_time = get_ms()
    game_state.last_spawn = get_ms()

    print("进入游戏主循环...")

    while true do
        local current_time = get_ms()
        local dt = current_time - last_frame_time
        last_frame_time = current_time

        if game_state.is_running then
            -- 物理更新
            if game_state.is_jumping or game_state.player_y < baseline_y then
                game_state.player_vy = game_state.player_vy + GRAVITY
                game_state.player_y = game_state.player_y + game_state.player_vy
                -- 地面碰撞
                if game_state.player_y >= baseline_y then
                    game_state.player_y = baseline_y
                    game_state.player_vy = 0
                    game_state.is_jumping = false
                end
                set_pos_int(player_obj, 60, game_state.player_y - PLAYER_SIZE)
            end

            -- 根据分数增加难度
            game_state.speed_multiplier = 1.0 + (game_state.score * 0.05)
            local current_speed = BASE_OBSTACLE_SPEED * game_state.speed_multiplier
            local current_spawn_interval = math.max(MIN_SPAWN_INTERVAL,
                BASE_SPAWN_INTERVAL - (game_state.score * 50))

            -- 生成障碍物
            if current_time - game_state.last_spawn > current_spawn_interval then
                local obs_obj, obs_height, obs_width, obs_y = create_obstacle(scr, screen_w)
                table.insert(game_state.obstacles, {
                    obj = obs_obj,
                    x = screen_w,
                    y = obs_y,    --【修复】保存正确Y，永远不从控件读取Y
                    height = obs_height,
                    width = obs_width,
                    passed = false,
                })
                game_state.last_spawn = current_time
            end

            -- 更新障碍物
            for i = #game_state.obstacles, 1, -1 do
                local obs = game_state.obstacles[i]
                obs.x = obs.x - current_speed
                --【修复】Y直接使用内存保存的值，不调用get_pos获取Y，避免LVGL返回错误值飞到天上
                set_pos_int(obs.obj, obs.x, obs.y)

                -- 碰撞检测（直接传入内存保存的坐标，不读控件）
                if check_collision(obs.obj, obs.x, obs.y, obs.width, obs.height) then
                    show_game_over()
                    -- 清除所有障碍物
                    for _, o in ipairs(game_state.obstacles) do
                        o.obj:delete()
                    end
                    game_state.obstacles = {}
                    break
                end

                -- 计分
                if not obs.passed and obs.x + obs.width < 60 then
                    obs.passed = true
                    game_state.score = game_state.score + 1
                    score_label:set_text("分数: " .. game_state.score)
                end

                -- 移除屏幕外的障碍物
                if obs.x < -obs.width then
                    obs.obj:delete()
                    table.remove(game_state.obstacles, i)
                end
            end
        end

        -- 更新云朵动画
        if current_time - last_cloud_update > 50 then
            last_cloud_update = current_time
            for _, cloud in ipairs(clouds) do
                local cx = cloud.x - cloud.speed
                if cx < -80 then
                    cx = screen_w + 10
                end
                cloud.x = cx
                local cox, coy = cloud.obj:get_pos()
                set_pos_int(cloud.obj, cx, coy)
            end
        end

        -- 处理 LVGL 事件
        lvgl.process_events(16)
    end

    -- 清理
    lvgl.indev_unregister("touch")
    lvgl.deinit()
end
-- 运行游戏
main()


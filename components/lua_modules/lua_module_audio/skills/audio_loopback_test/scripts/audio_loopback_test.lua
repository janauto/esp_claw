local lvgl = require("lvgl")
local board_manager = require("board_manager")
local audio = require("audio")
local delay = require("delay")

-- ====== 初始化设备 ======
pcall(function() board_manager.init_device("lcd_touch") end)
pcall(function() board_manager.init_device("audio_dac") end)
pcall(function() board_manager.init_device("audio_adc") end)

-- ====== 初始化 LVGL ======
lvgl.init({
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
})

-- 注册触摸
local touch_handle = board_manager.get_lcd_touch_handle("lcd_touch")
if touch_handle then
    lvgl.indev_register("touch", touch_handle)
    print("触摸已注册")
else
    print("警告: 无法获取触摸句柄")
end

-- ====== 获取音频参数 ======
local output_codec, output_rate, output_channels, output_bits =
    board_manager.get_audio_codec_output_params("audio_dac")
local input_codec, input_rate, input_channels, input_bits, init_gain =
    board_manager.get_audio_codec_input_params("audio_adc")

-- 兜底默认值
if not output_rate or output_rate <= 0 then output_rate = 48000 end
if not output_channels or output_channels <= 0 then output_channels = 2 end
if not output_bits or output_bits <= 0 then output_bits = 16 end
if not input_rate or input_rate <= 0 then input_rate = 16000 end
if not input_channels or input_channels <= 0 then input_channels = 1 end
if not input_bits or input_bits <= 0 then input_bits = 16 end

-- 统一采样率，以输出为准
local common_rate = output_rate
print(string.format("输出: %dHz/%dch/%dbit", output_rate, output_channels, output_bits))
print(string.format("输入: %dHz/%dch/%dbit (将使用 %dHz 打开)", input_rate, input_channels, input_bits, common_rate))

-- ====== 打开音频设备 ======
local output = assert(audio.new_output({
    codec = output_codec,
    sample_rate = common_rate,
    channels = output_channels,
    bits = output_bits,
    volume = 80,
}))
print("输出设备已打开")

local input = assert(audio.new_input({
    codec = input_codec,
    sample_rate = common_rate,
    channels = input_channels,
    bits = output_bits,
    volume = 70,
}))
print("输入设备已打开")

-- 读取实际参数
local out_info = output:info()
local in_info = input:info()
print(string.format("输出实际: %dHz/%dch/%dbit", out_info.sample_rate, out_info.channels, out_info.bits))
print(string.format("输入实际: %dHz/%dch/%dbit", in_info.sample_rate, in_info.channels, in_info.bits))

local act_rate = out_info.sample_rate
local act_out_ch = out_info.channels
local act_in_ch = in_info.channels
local act_bits = out_info.bits
local bytes_per_sample = act_bits / 8

-- 10ms PCM 块大小
local chunk_ms = 10
local in_frame_bytes = act_in_ch * bytes_per_sample
local out_frame_bytes = act_out_ch * bytes_per_sample
local in_chunk_bytes = math.floor(act_rate * chunk_ms / 1000 * in_frame_bytes)
local out_chunk_bytes = math.floor(act_rate * chunk_ms / 1000 * out_frame_bytes)
print(string.format("回环块: 读%d字节/写%d字节 (每%dms)", in_chunk_bytes, out_chunk_bytes, chunk_ms))

-- ====== 回环状态 ======
local loop_running = false

-- ====== 预编译声道转换策略 ======
-- 策略: "copy" = 输入输出声道一致, "mono2stereo" = 单→双, "stereo2mono" = 双→单
local conv_strategy
if act_in_ch == act_out_ch then
    conv_strategy = "copy"
elseif act_in_ch == 1 and act_out_ch == 2 then
    conv_strategy = "mono2stereo"
elseif act_in_ch == 2 and act_out_ch == 1 then
    conv_strategy = "stereo2mono"
else
    conv_strategy = "copy"  -- 兜底
end
print("声道转换策略: " .. conv_strategy)

-- 声道转换函数（使用 string.gsub 在 C 层面批量处理，避免逐样本 Lua 循环）
local function convert_channels(pcm)
    if conv_strategy == "mono2stereo" then
        -- 16bit 单声道 → 立体声：将每 2 字节样本复制一份
        -- "(..)" 匹配任意 2 字节, "%1%1" 将其重复两次（左右声道相同）
        return string.gsub(pcm, "(..)", "%1%1")
    elseif conv_strategy == "stereo2mono" then
        -- 16bit 立体声 → 单声道：取左声道，丢弃右声道
        -- "(..)(..)" 匹配左+右共 4 字节, "%1" 只保留左声道
        return string.gsub(pcm, "(..)(..)", "%1")
    end
    return pcm  -- copy 策略，原样返回
end

-- ====== 创建 UI ======
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#1a1a2e" })

-- 标题
local title = lvgl.label(scr, {
    text = "音频回环测试",
    align = "top_mid",
    y = 25,
    text_color = "#e0e0e0",
})
title:set_style({ font_size = 28 })

-- 状态标签
local status_label = lvgl.label(scr, {
    text = "已停止",
    align = "center",
    y = -30,
    text_color = "#aaaaaa",
})
status_label:set_style({ font_size = 20 })

-- 回环开关按钮
local loop_btn = lvgl.button(scr, {
    text = "开始回环",
    align = "center",
    w = 200,
    h = 60,
    bg_color = "#2ecc71",
    text_color = "#ffffff",
    radius = 14,
})
loop_btn:set_style({ font_size = 22 })

-- 底部提示
local hint = lvgl.label(scr, {
    text = "实时: 麦克风 → 扬声器 (" .. chunk_ms .. "ms/块)",
    align = "bottom_mid",
    y = -15,
    text_color = "#666666",
})
hint:set_style({ font_size = 14 })

-- ====== 按钮回调 ======
loop_btn:on("clicked", function()
    loop_running = not loop_running
    if loop_running then
        loop_btn:set_text("停止回环")
        loop_btn:set_style({ bg_color = "#e74c3c" })
        status_label:set_text("回环中...")
        status_label:set_style({ text_color = "#2ecc71" })
        print(">>> 回环开始")
    else
        loop_btn:set_text("开始回环")
        loop_btn:set_style({ bg_color = "#2ecc71" })
        status_label:set_text("已停止")
        status_label:set_style({ text_color = "#aaaaaa" })
        print(">>> 回环停止")
    end
end)

-- ====== 加载屏幕 ======
scr:load()
print("UI 已加载，等待用户操作...")

-- ====== 主循环（优化版） ======
-- 核心思路：
--   1. 声道转换用 string.gsub 批量处理（C 层面，比逐样本 Lua 循环快 100 倍以上）
--   2. 每次 LVGL 滴答后处理 2 个音频块（20ms 音频），摊薄 LVGL 开销
--   3. 回环期间 LVGL 超时缩短为 2ms，优先保障音频吞吐
--   4. 去除 delay.delay_ms(1)，避免与 LVGL 内部 tick 争抢
--   5. 只在回环停止时累加空闲计数，避免回环期间误超时
local idle_count = 0
local max_idle = 600  -- 约 60 秒无触摸且回环停止后退出
local chunks_per_tick = 2  -- 每次处理 2 个音频块

while idle_count < max_idle do
    -- 回环运行时用短超时，空闲时用长超时
    local lvgl_timeout = loop_running and 2 or 10
    local n = lvgl.process_events(lvgl_timeout)

    if n > 0 then
        idle_count = 0
    elseif not loop_running then
        idle_count = idle_count + 1
    end

    if loop_running then
        for _ = 1, chunks_per_tick do
            local ok, pcm = pcall(function()
                return input:read(in_chunk_bytes)
            end)
            if ok and pcm and #pcm >= in_chunk_bytes then
                -- 每100次循环打印一次读取成功日志
                read_count = (read_count or 0) + 1
                if read_count == 1 or read_count % 100 == 0 then
                    print(string.format("✓ 读取成功: 已读取 %d 次，每次 %d 字节", read_count, #pcm))
                end

                -- 高速声道转换（string.gsub 在 C 层面批量替换）
                pcm = convert_channels(pcm)
                -- 写入扬声器
                pcall(function()
                    output:write(pcm)
                end)
            else
                -- 读取失败或数据为空
                zero_count = (zero_count or 0) + 1
                if zero_count == 1 or zero_count % 100 == 0 then
                    print(string.format("✗ 读取失败: 已失败 %d 次，ok=%s, pcm_len=%s",
                          zero_count, tostring(ok), pcm and tostring(#pcm) or "nil"))
                end
            end
        end
    else
        delay.delay_ms(10)
    end
end

print("超时退出")

-- ====== 清理 ======
loop_running = false
input:close()
output:close()
lvgl.deinit()
print("资源已释放")

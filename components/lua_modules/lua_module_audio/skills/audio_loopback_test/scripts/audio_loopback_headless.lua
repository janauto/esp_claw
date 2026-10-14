-- 音频回环测试（无UI，纯后台）
-- Used by WorkBuddy's native C UI. Keep this script display-free so the
-- WorkBuddy screen remains visible while audio loopback runs.

local audio = require("audio")
local board_manager = require("board_manager")

local CONTROL_FILE = "/ramfs/workbuddy_audio_control"
local RUNNING_FLAG = "/ramfs/workbuddy_audio_running"
local SHARED_MIC_AREA = "workbuddy_mic"
local SHARED_SPK_AREA = "workbuddy_spk"
local AUDIO_CHUNK_MS = 10

local INPUT_PROFILE_CURRENT = "current"
local INPUT_PROFILE_SPEECH_16K_MONO_16 = "speech_16k_mono_16"
local OUTPUT_PROFILE_CURRENT = "current"
local OUTPUT_PROFILE_SPEECH_16K_MONO_16 = "speech_16k_mono_16"
local OUTPUT_PROFILE_48K_STEREO_16 = "48k_stereo_16"
-- Macro-style input profile:
--   INPUT_PROFILE_CURRENT keeps the current hardware-friendly capture format.
--   INPUT_PROFILE_SPEECH_16K_MONO_16 opens mic as 16kHz/mono/16-bit PCM.
-- local WORKBUDDY_INPUT_PROFILE = INPUT_PROFILE_CURRENT
local WORKBUDDY_INPUT_PROFILE = INPUT_PROFILE_SPEECH_16K_MONO_16
-- Macro-style output profile:
--   OUTPUT_PROFILE_CURRENT keeps the board's default speaker format.
--   OUTPUT_PROFILE_SPEECH_16K_MONO_16 opens speaker as 16kHz/mono/16-bit PCM.
--   OUTPUT_PROFILE_48K_STEREO_16 opens speaker as 48kHz/stereo/16-bit PCM.
-- local WORKBUDDY_OUTPUT_PROFILE = OUTPUT_PROFILE_CURRENT
local WORKBUDDY_OUTPUT_PROFILE = OUTPUT_PROFILE_SPEECH_16K_MONO_16

local audio_ctx = {
    mic = {
        pcm = nil,
        bytes = 0,
        seq = 0,
        sample_rate = 0,
        channels = 0,
        bits = 0,
    },
    spk = {
        pcm = nil,
        bytes = 0,
        seq = 0,
        sample_rate = 0,
        channels = 0,
        bits = 0,
        play_pcm = nil,
        play_offset = 1,
        play_chunk_bytes = 0,
    },
}
local mic_area = audio_ctx.mic
local spk_area = audio_ctx.spk
local last_external_spk_seq = 0

local function mark_running(running)
    if running then
        local flag = io.open(RUNNING_FLAG, "w")
        if flag then
            flag:write("1")
            flag:close()
        end
    else
        os.remove(RUNNING_FLAG)
    end
end

local function read_stop_requested()
    local f = io.open(CONTROL_FILE, "r")
    if not f then
        return false
    end
    local cmd = f:read("*l")
    f:close()
    return cmd == "stop"
end

local function setup_audio_areas(in_info, out_info)
    mic_area.sample_rate = in_info.sample_rate
    mic_area.channels = in_info.channels
    mic_area.bits = in_info.bits
    spk_area.sample_rate = out_info.sample_rate
    spk_area.channels = out_info.channels
    spk_area.bits = out_info.bits
    spk_area.play_chunk_bytes = math.floor(
        out_info.sample_rate * AUDIO_CHUNK_MS / 1000 * out_info.channels * (out_info.bits / 8)
    )
end

local function area_format(area)
    return {
        sample_rate = area.sample_rate,
        channels = area.channels,
        bits = area.bits,
    }
end

local function process_mic_area(mic)
    -- mic.pcm is raw PCM just captured from the microphone.
    -- Add mic-side processing here: VAD, gain, denoise, ASR feed, recording, etc.
    if mic.pcm and mic.bytes > 0 then
        pcall(function()
            audio.shared_write(SHARED_MIC_AREA, mic.pcm, area_format(mic))
        end)
    end
    return mic.pcm
end

local function process_spk_area(spk)
    -- spk.pcm is raw PCM already formatted for the speaker.
    -- Add speaker-side processing here: mix TTS, mute gate, volume shaping, etc.
    return spk.pcm
end

local function convert_channels_factory(input_channels, output_channels, log_strategy)
    local strategy = "copy"
    if input_channels == 1 and output_channels == 2 then
        strategy = "mono2stereo"
    elseif input_channels == 2 and output_channels == 1 then
        strategy = "stereo2mono"
    end
    if log_strategy ~= false then
        print("声道转换策略: " .. strategy)
    end

    if strategy == "mono2stereo" then
        return function(pcm)
            return string.gsub(pcm, "(..)", "%1%1")
        end
    elseif strategy == "stereo2mono" then
        return function(pcm)
            return string.gsub(pcm, "(..)(..)", "%1")
        end
    end
    return function(pcm)
        return pcm
    end
end

local function convert_16bit_channels(pcm, input_channels, output_channels)
    if input_channels == output_channels then
        return pcm
    elseif input_channels == 1 and output_channels == 2 then
        return string.gsub(pcm, "(..)", "%1%1")
    elseif input_channels == 2 and output_channels == 1 then
        return string.gsub(pcm, "(..)(..)", "%1")
    end
    return pcm
end

local function resample_16bit_pcm(pcm, src_rate, channels, dst_rate)
    if src_rate == dst_rate then
        return pcm
    end

    local frame_bytes = channels * 2
    local in_frames = #pcm // frame_bytes
    if in_frames <= 0 then
        return pcm
    end

    local out_frames = math.floor((in_frames * dst_rate + (src_rate // 2)) / src_rate)
    local out = {}
    for i = 0, out_frames - 1 do
        local src_frame = math.floor((i * src_rate) / dst_rate)
        if src_frame >= in_frames then
            src_frame = in_frames - 1
        end
        local pos = src_frame * frame_bytes + 1
        out[#out + 1] = string.sub(pcm, pos, pos + frame_bytes - 1)
    end
    return table.concat(out)
end

local function convert_pcm_to_speaker(pcm, src)
    local src_rate = (src.sample_rate and src.sample_rate > 0) and src.sample_rate or spk_area.sample_rate
    local src_channels = (src.channels and src.channels > 0) and src.channels or spk_area.channels
    local src_bits = (src.bits and src.bits > 0) and src.bits or spk_area.bits

    if src_bits ~= 16 or spk_area.bits ~= 16 then
        return pcm
    end

    pcm = resample_16bit_pcm(pcm, src_rate, src_channels, spk_area.sample_rate)
    pcm = convert_16bit_channels(pcm, src_channels, spk_area.channels)
    return pcm
end

local function read_external_spk_area()
    local pcm, info = audio.shared_read(SHARED_SPK_AREA, {
        after_seq = last_external_spk_seq,
        clear = true,
    })
    if not pcm or not info then
        return false
    end

    last_external_spk_seq = info.seq or last_external_spk_seq
    spk_area.play_pcm = convert_pcm_to_speaker(pcm, info)
    spk_area.play_offset = 1
    spk_area.pcm = nil
    spk_area.bytes = #spk_area.play_pcm
    spk_area.seq = spk_area.seq + 1
    return true
end

local function capture_mic_area(input_dev, read_bytes)
    local read_ok, pcm = pcall(function()
        return input_dev:read(read_bytes)
    end)

    if not read_ok or not pcm or #pcm < read_bytes then
        return false, read_ok, pcm
    end

    mic_area.pcm = pcm
    mic_area.bytes = #pcm
    mic_area.seq = mic_area.seq + 1
    return true, true, pcm
end

local function prepare_spk_area()
    process_mic_area(mic_area)

    if not spk_area.play_pcm or spk_area.play_offset > #spk_area.play_pcm then
        if not read_external_spk_area() then
            spk_area.pcm = nil
            spk_area.bytes = 0
            return false
        end
    end

    local chunk_bytes = spk_area.play_chunk_bytes
    if chunk_bytes <= 0 then
        chunk_bytes = #spk_area.play_pcm
    end
    local start_pos = spk_area.play_offset
    local end_pos = math.min(start_pos + chunk_bytes - 1, #spk_area.play_pcm)
    spk_area.pcm = string.sub(spk_area.play_pcm, start_pos, end_pos)
    spk_area.play_offset = end_pos + 1
    spk_area.pcm = process_spk_area(spk_area)
    spk_area.bytes = spk_area.pcm and #spk_area.pcm or 0
    return spk_area.bytes > 0
end

local function play_spk_area(output_dev)
    if not spk_area.pcm or spk_area.bytes == 0 then
        return false
    end
    return pcall(function()
        output_dev:write(spk_area.pcm)
    end)
end

local function close_device(dev)
    if dev then
        pcall(function()
            dev:close()
        end)
    end
end

local input
local output

local function apply_input_profile(input_rate, input_channels, input_bits, output_rate)
    if WORKBUDDY_INPUT_PROFILE == INPUT_PROFILE_SPEECH_16K_MONO_16 then
        return 16000, 1, 16
    end
    return output_rate, input_channels, input_bits
end

local function apply_output_profile(output_rate, output_channels, output_bits)
    if WORKBUDDY_OUTPUT_PROFILE == OUTPUT_PROFILE_SPEECH_16K_MONO_16 then
        return 16000, 1, 16
    elseif WORKBUDDY_OUTPUT_PROFILE == OUTPUT_PROFILE_48K_STEREO_16 then
        return 48000, 2, 16
    end
    return output_rate, output_channels, output_bits
end

local ok, err = pcall(function()
    print("=== 音频回环后台模式启动 ===")

    pcall(function() board_manager.init_device("audio_dac") end)
    pcall(function() board_manager.init_device("audio_adc") end)

    local output_codec, output_rate, output_channels, output_bits =
        board_manager.get_audio_codec_output_params("audio_dac")
    local input_codec, input_rate, input_channels, input_bits =
        board_manager.get_audio_codec_input_params("audio_adc")

    if not output_rate or output_rate <= 0 then output_rate = 48000 end
    if not output_channels or output_channels <= 0 then output_channels = 2 end
    if not output_bits or output_bits <= 0 then output_bits = 16 end
    if not input_rate or input_rate <= 0 then input_rate = 16000 end
    if not input_channels or input_channels <= 0 then input_channels = 1 end
    if not input_bits or input_bits <= 0 then input_bits = 16 end

    local input_open_rate, input_open_channels, input_open_bits =
        apply_input_profile(input_rate, input_channels, input_bits, output_rate)
    local output_open_rate, output_open_channels, output_open_bits =
        apply_output_profile(output_rate, output_channels, output_bits)

    print(string.format("输出: %dHz/%dch/%dbit (profile=%s, 将使用 %dHz/%dch/%dbit 打开)",
        output_rate, output_channels, output_bits, WORKBUDDY_OUTPUT_PROFILE,
        output_open_rate, output_open_channels, output_open_bits))
    print(string.format("输入: %dHz/%dch/%dbit (profile=%s, 将使用 %dHz/%dch/%dbit 打开)",
        input_rate, input_channels, input_bits, WORKBUDDY_INPUT_PROFILE,
        input_open_rate, input_open_channels, input_open_bits))

    output = assert(audio.new_output({
        codec = output_codec,
        sample_rate = output_open_rate,
        channels = output_open_channels,
        bits = output_open_bits,
        volume = 80,
    }))
    print("输出设备已打开")

    input = assert(audio.new_input({
        codec = input_codec,
        sample_rate = input_open_rate,
        channels = input_open_channels,
        bits = input_open_bits,
        volume = 70,
    }))
    print("输入设备已打开")

    local out_info = output:info()
    local in_info = input:info()
    print(string.format("输出实际: %dHz/%dch/%dbit",
        out_info.sample_rate, out_info.channels, out_info.bits))
    print(string.format("输入实际: %dHz/%dch/%dbit",
        in_info.sample_rate, in_info.channels, in_info.bits))

    local bytes_per_sample = in_info.bits / 8
    local in_frame_bytes = in_info.channels * bytes_per_sample
    local in_chunk_bytes = math.floor(in_info.sample_rate * AUDIO_CHUNK_MS / 1000 * in_frame_bytes)
    print(string.format("回环块: 读%d字节 (每%dms)", in_chunk_bytes, AUDIO_CHUNK_MS))

    convert_channels_factory(in_info.channels, out_info.channels)
    setup_audio_areas(in_info, out_info)

    local success_count = 0
    local zero_count = 0
    local control_check = 0
    local chunks_per_check = 100

    os.remove(CONTROL_FILE)
    mark_running(true)
    print(">>> 回环开始（后台模式）")

    while true do
        local has_mic_data, read_ok, pcm = capture_mic_area(input, in_chunk_bytes)

        if has_mic_data then
            success_count = success_count + 1
            if prepare_spk_area() then
                play_spk_area(output)
            end

            if success_count == 1 or (success_count % 100) == 0 then
                print(string.format("✓ 音频区: mic_seq=%d mic_bytes=%d spk_seq=%d spk_bytes=%d",
                    mic_area.seq, mic_area.bytes, spk_area.seq, spk_area.bytes))
            end
        else
            zero_count = zero_count + 1
            if zero_count == 1 or (zero_count % 100) == 0 then
                print(string.format("✗ 读取失败: 已失败 %d 次，ok=%s, pcm_len=%s",
                    zero_count, tostring(read_ok), pcm and tostring(#pcm) or "nil"))
            end
        end

        control_check = control_check + 1
        if (control_check % chunks_per_check) == 0 and read_stop_requested() then
            print("收到停止信号")
            break
        end
    end

    print(">>> 回环停止（后台模式）")
    print(string.format("统计: 成功=%d, 零读=%d", success_count, zero_count))
end)

close_device(input)
close_device(output)
mark_running(false)
os.remove(CONTROL_FILE)

if not ok then
    print("音频回环后台模式失败: " .. tostring(err))
    error(err)
end

print("音频设备已关闭")

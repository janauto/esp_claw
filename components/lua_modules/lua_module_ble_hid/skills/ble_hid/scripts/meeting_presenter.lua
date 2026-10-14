--[[
Description: 会议翻页笔。ADC 四按键 + 触摸按钮 -> BLE HID 翻页。

    VOLUME_UP   -> 下一页 (HID PAGE_DOWN)
    VOLUME_DOWN -> 上一页 (HID PAGE_UP)
    MODE        -> 退出，回到之前的界面

按键含义按放映软件惯例映射：PowerPoint / WPS / Keynote / Google Slides 都用
PAGE_DOWN 翻到下一张、PAGE_UP 回到上一张，所以插上就能直接用。

前提：BLE 由 ble_host_bridge 常驻托管（attached 模式），所以本脚本不调用
      ble_hid.init()/start()，它们在 attached 模式下是空操作。
      HID 报文只发给「已加密链路」的那个 peer，也就是从系统蓝牙设置里
      配对过 esp-claw-host 的那台机器。发不出去时看屏幕底部的状态行。

退出：lvgl.init() 开的是带 RESTORE_DEFAULT_ON_RELEASE 的独占显示会话，
      所以 lvgl.deinit() 会自动把屏幕交还给启动本脚本的那个界面。
--]]
local button = require("button")
local lvgl = require("lvgl")
local delay = require("delay")
local ble_hid = require("ble_hid")

-- 配色跟随 SquareLine 界面，保持整机观感一致
local C_BG        = "#000000"
local C_PANEL     = "#080C08"
local C_ACCENT    = "#F0C428"
local C_ACCENT_HI = "#FFE45B"
local C_DIM       = "#384038"
local C_TEXT      = "#FAFBF5"
local C_TEXT_DIM  = "#788078"
local C_OK        = "#00FF88"
local C_WARN      = "#FFCC00"

lvgl.init({
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
})

-- LVGL 的基类构造函数给每个 lv_obj 都置上 SCROLLABLE，滚动条模式默认 auto，
-- 所以纯装饰用的 container 也能被拖动、并在拖动时冒出滚动条。这个界面是固定
-- 布局，没有任何东西需要滚动，于是统一走这个包装：dir=none 关掉滚动手势，
-- scrollbar=off 关掉滚动条绘制。
local function panel(parent, opts)
    local obj = lvgl.container(parent, opts)
    obj:set_scroll({ dir = "none", scrollbar = "off" })
    return obj
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C_BG, bg_opa = 255 })
-- 屏幕本身也是一个 lv_obj，同样默认可滚动
scr:set_scroll({ dir = "none", scrollbar = "off" })

-- 横屏（800x480）时翻页键在页码卡片左右；竖屏（480x800）时卡片上移，两键并排在下方
local SCREEN_W, SCREEN_H = scr:get_size()
local PORTRAIT = SCREEN_H > SCREEN_W

---------------------------------------------------------------- 顶部标题栏
local top_bar = panel(scr, { w = SCREEN_W, h = 56, align = "top_mid", y = 0 })
top_bar:set_style({ bg_color = C_PANEL, bg_opa = 255, radius = 0, border_width = 0 })

-- 左侧那一小条黄色装饰，和其他界面的状态栏对齐
local top_tick = panel(top_bar, { w = 9, h = 34, align = "left_mid", x = 14 })
top_tick:set_style({ bg_color = C_ACCENT_HI, bg_opa = 255, radius = 2, border_width = 0 })

lvgl.label(top_bar, {
    text = "会议翻页笔",
    align = "left_mid",
    x = 40,
    text_color = C_TEXT,
})

local link_dot = panel(top_bar, { w = 16, h = 16, align = "right_mid", x = -20 })
link_dot:set_style({ bg_color = C_DIM, bg_opa = 255, radius = 75, border_width = 0 })

---------------------------------------------------------------- 中间页码卡片
local page_card = panel(scr, { w = 260, h = 168, align = "center", y = PORTRAIT and -110 or -26 })
page_card:set_style({
    bg_color = C_PANEL,
    bg_opa = 255,
    radius = 20,
    border_color = C_ACCENT_HI,
    border_width = 6,
})

-- 卡片顶部的高亮横条，和首页应用卡片同一套视觉语言
local card_tab = panel(page_card, { w = 152, h = 13, align = "top_mid", y = 14 })
card_tab:set_style({ bg_color = C_ACCENT_HI, bg_opa = 255, radius = 10, border_width = 0 })

local page_label = lvgl.label(page_card, {
    text = "1",
    align = "center",
    y = 6,
    text_color = C_ACCENT,
})

local page_hint = lvgl.label(page_card, {
    text = "本机计数",
    align = "bottom_mid",
    y = -14,
    text_color = C_TEXT_DIM,
})

---------------------------------------------------------------- 左右翻页按钮
-- 用 container + label 而不是 lvgl.button，方便精确控制圆角/描边配色
local function make_key(parent, opts)
    local box = panel(parent, {
        w = opts.w,
        h = opts.h,
        align = opts.align,
        x = opts.x,
        y = opts.y,
    })
    box:set_style({
        bg_color = C_PANEL,
        bg_opa = 255,
        radius = 14,
        border_color = C_DIM,
        border_width = 4,
    })

    local title = lvgl.label(box, {
        text = opts.title,
        align = "center",
        y = -12,
        text_color = C_TEXT,
    })

    lvgl.label(box, {
        text = opts.subtitle,
        align = "center",
        y = 18,
        text_color = C_TEXT_DIM,
    })

    return { box = box, title = title }
end

local prev_key = make_key(scr, {
    w = 210, h = 104, align = "center",
    x = PORTRAIT and -112 or -258, y = PORTRAIT and 120 or -26,
    title = "< 上一页", subtitle = "VOL-",
})

local next_key = make_key(scr, {
    w = 210, h = 104, align = "center",
    x = PORTRAIT and 112 or 258, y = PORTRAIT and 120 or -26,
    title = "下一页 >", subtitle = "VOL+",
})

---------------------------------------------------------------- 底部状态栏
local bottom_bar = panel(scr, { w = SCREEN_W, h = 72, align = "bottom_mid", y = 0 })
bottom_bar:set_style({
    bg_color = C_PANEL,
    bg_opa = 255,
    radius = 0,
    border_color = C_DIM,
    border_width = 6,
})

local exit_key = panel(bottom_bar, { w = 168, h = 44, align = "left_mid", x = 20 })
exit_key:set_style({
    bg_color = C_PANEL,
    bg_opa = 255,
    radius = 10,
    border_color = C_ACCENT,
    border_width = 3,
})
lvgl.label(exit_key, { text = "退出  MODE", align = "center", text_color = C_ACCENT })

-- 竖屏底栏放不下退出键和状态文字，状态单独放在底栏上方一行
local hid_label = lvgl.label(PORTRAIT and scr or bottom_bar, {
    text = "HID: 等待配对 esp-claw-host",
    align = PORTRAIT and "bottom_mid" or "right_mid",
    x = PORTRAIT and 0 or -24,
    y = PORTRAIT and -96 or 0,
    text_color = C_WARN,
})

---------------------------------------------------------------- 状态
local page = 1
local running = true

-- 非 nil 时优先显示最近一次发送结果，过期后回到链路状态
local hid_msg = nil
local hid_msg_ticks = 0

local function set_hid_msg(text, color)
    hid_msg = { text = text, color = color }
    hid_msg_ticks = 60      -- 约 1.2s
end

local function set_page(value)
    if value < 1 then
        value = 1
    end
    page = value
    page_label:set_text(tostring(page))
end

-- 短暂高亮一下被触发的那个键，按键和触摸都走这里，反馈一致
local FLASH_TICKS = 12
local flash = {}

local function flash_key(key)
    key.box:set_style({ border_color = C_ACCENT_HI, bg_color = C_DIM })
    flash[key] = FLASH_TICKS
end

local function flash_tick()
    for key, ticks in pairs(flash) do
        ticks = ticks - 1
        if ticks <= 0 then
            key.box:set_style({ border_color = C_DIM, bg_color = C_PANEL })
            flash[key] = nil
        else
            flash[key] = ticks
        end
    end
end

---------------------------------------------------------------- HID 动作
-- 上一轮可能被打断，先清掉可能卡住的按键
ble_hid.release_all()

local function send_key(keyname, desc)
    -- 参数校验类错误会 raise，用 pcall 兜住，发送失败只提示不中断
    local called, ok, err = pcall(ble_hid.key, keyname)
    if not called then
        set_hid_msg("HID 异常: " .. tostring(ok), "#FF5555")
        return false
    elseif ok then
        set_hid_msg("已发送: " .. desc, C_OK)
        return true
    else
        set_hid_msg("发送失败: " .. tostring(err), "#FF5555")
        return false
    end
end

local function go_next()
    flash_key(next_key)
    if send_key("PAGE_DOWN", "下一页") then
        set_page(page + 1)
    end
end

local function go_prev()
    flash_key(prev_key)
    if send_key("PAGE_UP", "上一页") then
        set_page(page - 1)
    end
end

local function quit()
    running = false
end

---------------------------------------------------------------- 触摸
next_key.box:on("clicked", go_next)
prev_key.box:on("clicked", go_prev)
exit_key:on("clicked", quit)

scr:load()

---------------------------------------------------------------- 实体按键
local ACTIONS = {
    VOLUME_UP   = go_next,
    VOLUME_DOWN = go_prev,
    MODE        = quit,
}
-- YAML button_labels 找不到时按硬件顺序兜底
local ORDER = { "VOLUME_UP", "VOLUME_DOWN", "MODE", "SET" }

local group = assert(button.open_group("adc_button_group"))
local labels = button.group_labels(group) or {}

local by_label = {}
for i, name in ipairs(labels) do
    by_label[name] = ACTIONS[name] or ACTIONS[ORDER[i]]
end

-- 用 single_click 而不是 press_down：翻别人的幻灯片，宁可多 100ms 也不要重复触发
button.on(group, "single_click", function(evt)
    local action = evt.label and by_label[evt.label]
    if action then
        action()
    end
end)

---------------------------------------------------------------- 主循环
local status_tick = 0

while running do
    button.dispatch()
    lvgl.process_events(10)
    flash_tick()

    if hid_msg then
        hid_label:set_text(hid_msg.text)
        hid_label:set_style({ text_color = hid_msg.color })
        hid_msg_ticks = hid_msg_ticks - 1
        if hid_msg_ticks <= 0 then
            hid_msg = nil
            status_tick = 0
        end
    else
        -- 链路状态约每 0.5s 刷一次，不必每帧查
        status_tick = status_tick - 1
        if status_tick <= 0 then
            status_tick = 50
            local st = ble_hid.status()
            if st.ready then
                hid_label:set_text("HID: 就绪")
                hid_label:set_style({ text_color = C_OK })
                link_dot:set_style({ bg_color = C_OK })
            elseif st.connected and not st.encrypted then
                hid_label:set_text("HID: 已连接，等待配对")
                hid_label:set_style({ text_color = C_WARN })
                link_dot:set_style({ bg_color = C_WARN })
            else
                hid_label:set_text("HID: 等待配对 esp-claw-host")
                hid_label:set_style({ text_color = C_WARN })
                link_dot:set_style({ bg_color = C_DIM })
            end
        end
    end

    delay.delay_ms(10)
end

---------------------------------------------------------------- 收尾
button.off(group)
button.close(group)
ble_hid.release_all()
lvgl.deinit()

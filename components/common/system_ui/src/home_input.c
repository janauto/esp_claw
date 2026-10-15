/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 自动验收用的触摸注入：一个虚拟 LVGL 指针设备 + 同一套系统手势识别。
 *
 * 状态由 LVGL 每次读取输入时按真实时间计算（不靠单独的定时器计数）：
 * 第一次读取一定是“按下”，按住时长到了才“抬起”。界面忙、读取间隔变长时，
 * 点按也不会因为定时器迟到而被拉长成长按，长按也一定会被看到按下。
 */
#include "home_priv.h"

#include <string.h>

#include "esp_log.h"

static struct {
    lv_indev_t *indev;
    lv_point_t pt;
    bool active;
    bool seen_press;
    int type;
    lv_point_t a, b;
    uint32_t ms;
    uint32_t start;
} TI;

static void hv_vin_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    bool pressed = false;

    if (TI.active) {
        uint32_t el = lv_tick_elaps(TI.start);
        if (!TI.seen_press) {
            TI.seen_press = true;
            TI.start = lv_tick_get();
            TI.pt = TI.a;
            pressed = true;
        } else if (el < TI.ms) {
            if (TI.type == 2) {
                /* 拖动：先停 80 ms 让控件认出按下，再按时间线性移动 */
                uint32_t move_t = el > 80 ? el - 80 : 0;
                uint32_t span = TI.ms > 80 ? TI.ms - 80 : 1;
                TI.pt.x = TI.a.x + (int32_t)((int64_t)(TI.b.x - TI.a.x) * move_t / span);
                TI.pt.y = TI.a.y + (int32_t)((int64_t)(TI.b.y - TI.a.y) * move_t / span);
            }
            pressed = true;
        } else {
            TI.pt = TI.type == 2 ? TI.b : TI.a;
            TI.active = false;
        }
        system_ui_touch_inject_sample(TI.pt.x, TI.pt.y, pressed);
    }
    data->point = TI.pt;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void hv_touch_inject_start(int type, int32_t x, int32_t y, int32_t x2, int32_t y2, uint32_t ms)
{
    if (!TI.indev) {
        TI.indev = lv_indev_create();
        lv_indev_set_type(TI.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(TI.indev, hv_vin_read);
        lv_indev_set_display(TI.indev, lv_display_get_default());
    }
    TI.type = type;
    TI.a.x = x;
    TI.a.y = y;
    TI.b.x = x2;
    TI.b.y = y2;
    TI.ms = ms < 40 ? 40 : ms > 5000 ? 5000 : ms;
    TI.seen_press = false;
    TI.active = true;
    ESP_LOGI(SYSTEM_UI_TAG, "HV_TOUCH type=%d %ld,%ld -> %ld,%ld ms=%lu", type, (long)x, (long)y,
             (long)x2, (long)y2, (unsigned long)TI.ms);
}

void hv_touch_inject(int type, int32_t x, int32_t y, int32_t x2, int32_t y2, uint32_t ms)
{
    hv_touch_inject_start(type, x, y, x2, y2, ms);
}

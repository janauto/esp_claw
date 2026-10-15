/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 自动验收用的触摸注入：一个虚拟 LVGL 指针设备 + 同一套系统手势识别。
 * 点按 / 长按 / 拖动按 20 ms 步进播放，和手指操作走完全相同的事件链路。
 */
#include "home_priv.h"

#include <string.h>

#include "esp_log.h"

static struct {
    lv_indev_t *indev;
    lv_timer_t *timer;
    lv_point_t pt;
    bool pressed;
    int type;
    lv_point_t a, b;
    uint32_t ms;
    uint32_t t;
} TI;

static void hv_vin_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = TI.pt;
    data->state = TI.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void hv_inject_step(lv_timer_t *timer)
{
    (void)timer;
    TI.t += 20;
    if (TI.t <= TI.ms) {
        if (TI.type == 2) {
            /* 拖动：先停 80 ms 让控件认出按下，再线性移动 */
            uint32_t move_t = TI.t > 80 ? TI.t - 80 : 0;
            uint32_t span = TI.ms > 80 ? TI.ms - 80 : 1;
            TI.pt.x = TI.a.x + (int32_t)((int64_t)(TI.b.x - TI.a.x) * move_t / span);
            TI.pt.y = TI.a.y + (int32_t)((int64_t)(TI.b.y - TI.a.y) * move_t / span);
        }
        TI.pressed = true;
        system_ui_touch_inject_sample(TI.pt.x, TI.pt.y, true);
    } else if (TI.pressed) {
        TI.pressed = false;
        system_ui_touch_inject_sample(TI.pt.x, TI.pt.y, false);
    } else if (TI.t > TI.ms + 200) {
        lv_timer_pause(TI.timer);
    }
}

void hv_touch_inject_start(int type, int32_t x, int32_t y, int32_t x2, int32_t y2, uint32_t ms)
{
    if (!TI.indev) {
        TI.indev = lv_indev_create();
        lv_indev_set_type(TI.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(TI.indev, hv_vin_read);
        lv_indev_set_display(TI.indev, lv_display_get_default());
    }
    if (!TI.timer) {
        TI.timer = lv_timer_create(hv_inject_step, 20, NULL);
    }
    TI.type = type;
    TI.a.x = x;
    TI.a.y = y;
    TI.b.x = x2;
    TI.b.y = y2;
    TI.pt = TI.a;
    TI.ms = ms < 40 ? 40 : ms > 5000 ? 5000 : ms;
    TI.t = 0;
    TI.pressed = true;
    system_ui_touch_inject_sample(x, y, true);
    lv_timer_reset(TI.timer);
    lv_timer_resume(TI.timer);
    ESP_LOGI(SYSTEM_UI_TAG, "HV_TOUCH type=%d %ld,%ld -> %ld,%ld ms=%lu", type, (long)x, (long)y,
             (long)x2, (long)y2, (unsigned long)TI.ms);
}

void hv_touch_inject(int type, int32_t x, int32_t y, int32_t x2, int32_t y2, uint32_t ms)
{
    hv_touch_inject_start(type, x, y, x2, y2, ms);
}

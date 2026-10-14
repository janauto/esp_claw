/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const lv_font_t ui_font_Font2;
extern const lv_font_t ui_font_Font3;

#define SYSTEM_UI_BOOT_PROGRESS_STEPS           25
#define SYSTEM_UI_BOOT_PROGRESS_STEP_DELAY_MS   24
#define SYSTEM_UI_BOOT_SETTLE_DELAY_MS          120

static lv_obj_t *ui_Screen4;
static lv_obj_t *ui_Container59;
static lv_obj_t *ui_Label18;
static lv_obj_t *ui_Bar2;
static lv_obj_t *ui_Label19;

static void system_ui_boot_set_progress_locked(int32_t value)
{
    if (ui_Bar2 != NULL) {
        lv_bar_set_value(ui_Bar2, value, LV_ANIM_OFF);
    }
}

esp_err_t system_ui_create_boot_locked(void)
{
    ui_Screen4 = lv_obj_create(NULL);
    ESP_RETURN_ON_FALSE(ui_Screen4 != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create boot screen failed");
    lv_obj_clear_flag(ui_Screen4, LV_OBJ_FLAG_SCROLLABLE);

    ui_Container59 = lv_obj_create(ui_Screen4);
    ESP_RETURN_ON_FALSE(ui_Container59 != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create boot container failed");
    lv_obj_remove_style_all(ui_Container59);
    lv_obj_set_width(ui_Container59, 804);
    lv_obj_set_height(ui_Container59, 490);
    lv_obj_set_x(ui_Container59, -1);
    lv_obj_set_y(ui_Container59, -5);
    lv_obj_set_align(ui_Container59, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Container59, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_Container59, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Container59, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Container59, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_img_recolor(ui_Container59, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(ui_Container59, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container59, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_Container59, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(ui_Container59, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(ui_Container59, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ui_Container59, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ui_Container59, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);

    ui_Label18 = lv_label_create(ui_Screen4);
    ESP_RETURN_ON_FALSE(ui_Label18 != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create boot title failed");
    lv_obj_set_width(ui_Label18, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label18, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Label18, 0);
    lv_obj_set_y(ui_Label18, -65);
    lv_obj_set_align(ui_Label18, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label18, "PISEN");
    lv_obj_set_style_text_color(ui_Label18, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label18, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label18, &ui_font_Font3, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Bar2 = lv_bar_create(ui_Screen4);
    ESP_RETURN_ON_FALSE(ui_Bar2 != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create boot progress failed");
    lv_bar_set_value(ui_Bar2, 0, LV_ANIM_OFF);
    lv_bar_set_start_value(ui_Bar2, 0, LV_ANIM_OFF);
    lv_obj_set_width(ui_Bar2, 360);
    lv_obj_set_height(ui_Bar2, 30);
    lv_obj_set_x(ui_Bar2, 0);
    lv_obj_set_y(ui_Bar2, 84);
    lv_obj_set_align(ui_Bar2, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ui_Bar2, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar2, lv_color_hex(0x3D403A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar2, lv_color_hex(0xF0C430), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar2, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    ui_Label19 = lv_label_create(ui_Screen4);
    ESP_RETURN_ON_FALSE(ui_Label19 != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create boot status failed");
    lv_obj_set_width(ui_Label19, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Label19, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Label19, -6);
    lv_obj_set_y(ui_Label19, 31);
    lv_obj_set_align(ui_Label19, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Label19, "SYSTEM CHECK");
    lv_obj_set_style_text_color(ui_Label19, lv_color_hex(0x7C817A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Label19, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Label19, &ui_font_Font2, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_ui.boot_screen = ui_Screen4;
    return ESP_OK;
}

void system_ui_delete_boot_locked(void)
{
    if (ui_Screen4 != NULL) {
        lv_obj_del(ui_Screen4);
    }
    s_ui.boot_screen = NULL;
    ui_Screen4 = NULL;
    ui_Container59 = NULL;
    ui_Label18 = NULL;
    ui_Bar2 = NULL;
    ui_Label19 = NULL;
}

void system_ui_run_boot_animation(void)
{
    for (int32_t i = 0; i <= SYSTEM_UI_BOOT_PROGRESS_STEPS; i++) {
        int32_t progress = (100 * i) / SYSTEM_UI_BOOT_PROGRESS_STEPS;
        if (system_ui_lock() == ESP_OK) {
            system_ui_boot_set_progress_locked(progress);
            system_ui_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_UI_BOOT_PROGRESS_STEP_DELAY_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(SYSTEM_UI_BOOT_SETTLE_DELAY_MS));
}

/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0 系统首页（HomeView）内部接口。
 *
 * 首页是固件原生绘制的系统外框：状态栏、软键栏、小组件页、底部应用栏、应用总览、
 * 控制中心（两种模式）、编辑首页与按住说话。主题只作用于「我的」（Lua 花园），
 * 首页不随主题变化。所有 hv_* 界面函数都要求调用方已持有 system_ui_lock()。
 */
#pragma once

#include "system_ui_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 设计 token */

#define HV_C_BG         0x000000
#define HV_C_ACCENT     0xF0C430
#define HV_C_ACCENT_INK 0x16130A
#define HV_C_TEXT       0xF2F4F8
#define HV_C_MUTED      0x9AA2B0
#define HV_C_FAINT      0x5C6370
#define HV_C_CARD       0x15171C
#define HV_C_CARD2      0x1F232B
#define HV_C_LINE       0x2C313A
#define HV_C_RED        0xE5484D
#define HV_C_GREEN      0x2FBF71
#define HV_C_BLUE       0x3B82F6

/* 480x800 竖屏设计画布；其他分辨率按宽度等比摆放（样机只有 480x800）。 */
#define HV_STATUS_H     44
#define HV_SOFTKEY_H    56
#define HV_PAD          24
#define HV_GAP          16
#define HV_CELL_W       208
#define HV_CELL_H       176
#define HV_LABEL_H      28
#define HV_ROW_H        (HV_CELL_H + HV_LABEL_H)
#define HV_PAGER_Y      84
#define HV_GRID_TOP     10      /* 页内网格上边距：给焦点框留出位置 */
#define HV_DOTS_Y       516
#define HV_DOCK_Y       548     /* 屏幕坐标；第 1 页内为 HV_DOCK_Y - HV_PAGER_Y */
#define HV_ICON         64

#define HV_MAX_PAGES    3
#define HV_MAX_PER_PAGE 4
#define HV_DOCK_MAX     4
#define HV_WID_LEN      40
#define HV_APP_ID_LEN   48
#define HV_TITLE_LEN    64

/* ------------------------------------------------------------- 字体 */

typedef struct {
    lv_font_t *f16;
    lv_font_t *f20;
    lv_font_t *f24;     /* = s_ui.font */
    lv_font_t *f28;
    lv_font_t *f40;
    lv_font_t *f64;
} hv_fonts_t;

extern hv_fonts_t g_hvf;
void hv_fonts_init(void);
void hv_fonts_deinit(void);
/* Montserrat 符号字体（LV_SYMBOL_*），按可用字号就近取。 */
const lv_font_t *hv_symbol_font(int px);

/* --------------------------------------------------------- 首页布局 */

typedef enum { HV_SZ_S = 0, HV_SZ_M = 1, HV_SZ_L = 2 } hv_sz_t;

typedef struct {
    char w[HV_WID_LEN];
    uint8_t sz;
} hv_item_t;

typedef struct {
    uint8_t pages;
    uint8_t n[HV_MAX_PAGES];
    hv_item_t it[HV_MAX_PAGES][HV_MAX_PER_PAGE];
    uint8_t dock_n;
    char dock[HV_DOCK_MAX][HV_APP_ID_LEN];
} hv_layout_t;

typedef struct {
    uint8_t col, row, cols, rows;
} hv_slot_t;

void hv_layout_default(hv_layout_t *l);
/* 按顺序先到先占地放入 2 列 x 2 行网格；放不下返回 false。 */
bool hv_layout_pack(const hv_item_t *items, int n, hv_slot_t *out);
esp_err_t hv_layout_load(hv_layout_t *l);
esp_err_t hv_layout_save(const hv_layout_t *l);
cJSON *hv_layout_to_json(const hv_layout_t *l);
esp_err_t hv_layout_from_json(const cJSON *j, hv_layout_t *out, char *err, size_t err_size);
bool hv_layout_find(const hv_layout_t *l, const char *w, int *page, int *index);
bool hv_layout_dock_has(const hv_layout_t *l, const char *app_id);
const char *hv_size_name(uint8_t sz);
int hv_size_from_name(const char *name);

/* --------------------------------------------------------- 小组件 */

#define HV_SZ_BIT(sz) (1u << (sz))

typedef struct {
    char id[HV_WID_LEN];
    char name[HV_TITLE_LEN];
    char desc[96];
    uint8_t sizes;          /* HV_SZ_BIT 组合 */
    char app[HV_APP_ID_LEN];/* 点按打开的应用，空 = 无 */
    bool fixed;             /* 不可移出（我的） */
    bool ai;                /* 来自 EPP 应用 */
} hv_widget_desc_t;

size_t hv_widgets_count(void);
const hv_widget_desc_t *hv_widgets_at(size_t i);
const hv_widget_desc_t *hv_widgets_find(const char *id);
void hv_widgets_rescan(void);       /* 重新扫描 <DATA>/widgets 目录里的应用小组件 */

lv_obj_t *hv_widget_create(lv_obj_t *parent, const char *wid, uint8_t sz, int32_t w, int32_t h);
void hv_widget_refresh(lv_obj_t *root);
void hv_widget_tick_second(lv_obj_t *root);
/* 语音卡片状态变化后让“语音助手”小组件跟着变。 */
void hv_widgets_note_voice(const char *heard, const char *reply);

/* --------------------------------------------------------- 应用目录 */

typedef enum {
    HV_KIND_SYSTEM = 0,     /* 原生 C 页面 */
    HV_KIND_BUILTIN_LUA,    /* 固件 SYSTEM 里的 Lua */
    HV_KIND_SKILL,          /* DATA/skills 旧格式应用 */
    HV_KIND_PACKAGE,        /* DATA/apps 应用包 */
    HV_KIND_GARDEN,         /* 我的 · 小磁（Lua 花园） */
} hv_kind_t;

typedef enum { HV_GROUP_BUILTIN = 0, HV_GROUP_OFFICIAL, HV_GROUP_MINE, HV_GROUP_COUNT } hv_group_t;

typedef struct {
    char id[HV_APP_ID_LEN];
    char title[HV_TITLE_LEN];
    hv_kind_t kind;
    hv_group_t group;
    bool ai;
    bool removable;
    bool renamable;
    uint32_t color;
    char glyph[16];         /* 图标：LV_SYMBOL 或文字 */
    bool glyph_symbol;      /* glyph 用符号字体 */
    char icon_path[SYSTEM_UI_PATH_MAX];  /* 应用包图标（RGB565 .bin），空 = 用 glyph */
    char script[SYSTEM_UI_PATH_MAX];     /* Lua 入口（绝对路径） */
    char args[96];
} hv_app_t;

size_t hv_catalog_refresh(void);
size_t hv_catalog_count(void);
const hv_app_t *hv_catalog_at(size_t i);
const hv_app_t *hv_catalog_find(const char *id);
size_t hv_catalog_group_count(hv_group_t g);
const char *hv_group_name(hv_group_t g);
const char *hv_kind_name(hv_kind_t k);
/* 打开应用：原生页面直接切换，Lua 走启动器（独占显示）。持锁调用。 */
esp_err_t hv_catalog_launch(const char *id, const char *args_json);
/* 卸载/改名：投递到 UI 事件任务执行（不持锁）。 */
esp_err_t hv_catalog_request_uninstall(const char *id);
esp_err_t hv_catalog_request_rename(const char *id, const char *title);
/* 由 UI 事件任务调用（不持锁）。 */
esp_err_t hv_catalog_uninstall_now(const char *id);
esp_err_t hv_catalog_rename_now(const char *id, const char *title);
/* 画应用图标（64 圆角方块），带 AI 角标。 */
lv_obj_t *hv_app_icon_create(lv_obj_t *parent, const hv_app_t *app, int32_t size, bool with_label);

/* --------------------------------------------------------- 平台接口 */
/* 固件实现见 home_platform.c；宿主预览用桩。 */

typedef enum { HV_MODE_CLIP = 0, HV_MODE_DOCK = 1 } hv_mode_t;

hv_mode_t hv_plat_mode_get(void);
esp_err_t hv_plat_mode_set(hv_mode_t mode);
bool hv_plat_phone_linked(void);         /* 手机已连接且订阅了语音通道 */
bool hv_plat_ble_connected(void);
bool hv_plat_wifi(char *ssid, size_t ssid_size);  /* 返回是否已连上 */
bool hv_plat_ai_ready(void);             /* 底座模式 AI 服务已配置 */
char *hv_plat_feed_read(const char *id, int64_t *age_ms);  /* 调用者 free */
char *hv_plat_read_text(const char *path, size_t max);     /* 调用者 free */
bool hv_plat_now(struct tm *out);         /* 时间未同步时返回 false */
int hv_plat_volume_get(void);
esp_err_t hv_plat_volume_set(int percent);
bool hv_plat_vibration_get(void);
void hv_plat_vibration_set(bool on);
const char *hv_plat_data_root(void);
const char *hv_plat_system_root(void);
bool hv_plat_file_exists(const char *path);
/* 语音：返回 ESP_OK 表示已开始收音 */
esp_err_t hv_plat_voice_begin(bool local);
esp_err_t hv_plat_voice_end(bool local, bool cancel);
esp_err_t hv_plat_voice_ask_text(const char *text);

/* ------------------------------------------------------------- 视图 */

typedef enum {
    HV_VIEW_HOME = 0,
    HV_VIEW_OVERVIEW,
    HV_VIEW_SHEET,
    HV_VIEW_CONFIRM,
    HV_VIEW_RENAME,
    HV_VIEW_INFO,
    HV_VIEW_CONTROL,
    HV_VIEW_EDIT,
    HV_VIEW_ADD,
    HV_VIEW_VOICE,
} hv_view_t;

typedef enum {
    HV_KEY_PREV = 0,    /* VOL- / ◀ */
    HV_KEY_NEXT,        /* VOL+ / ▶ */
    HV_KEY_M,           /* MODE 短按 */
    HV_KEY_M_LONG_START,/* MODE 按住 0.6 s */
    HV_KEY_M_LONG_END,  /* MODE 长按后抬起 */
    HV_KEY_SET,         /* SET 短按 */
    HV_KEY_SET_LONG,    /* SET 按住 0.6 s */
} hv_key_t;

lv_obj_t *hv_create_screen(void);
void hv_destroy(void);
bool hv_is_active(void);
hv_view_t hv_view(void);
const char *hv_view_name(hv_view_t v);
void hv_key(hv_key_t key);
void hv_on_show(void);                    /* 首页重新回到前台 */
void hv_toast(const char *text);
void hv_reload_apps(void);                /* 应用增删改后刷新目录与总览 */
void hv_open_overview(void);
void hv_close_overview(void);
void hv_open_control(void);
void hv_close_control(void);
void hv_enter_edit(void);
void hv_exit_edit(void);
void hv_go_home(void);
void hv_set_page(int page, bool anim);
int hv_page(void);
const hv_layout_t *hv_layout(void);
esp_err_t hv_apply_layout(const hv_layout_t *l, bool save);
void hv_refresh_status(void);
void hv_softkeys_set(const char *k1, const char *k2, const char *k3, const char *k4);
cJSON *hv_state_json(void);
/* 测试注入：屏幕坐标触摸。 */
void hv_touch_inject(int type, int32_t x, int32_t y, int32_t x2, int32_t y2, uint32_t ms);

/* 总览 / 应用菜单（home_overview.c） */
lv_obj_t *hv_overview_build(lv_obj_t *scr);
void hv_overview_refresh(void);
void hv_overview_key(hv_key_t key);
void hv_sheet_open(const char *app_id);
void hv_sheet_close(void);
void hv_sheet_key(hv_key_t key);
void hv_modal_key(hv_key_t key);          /* 确认 / 改名 / 详情 */
bool hv_modal_open(void);
void hv_modal_close(void);

/* 控制中心（home_control.c） */
lv_obj_t *hv_control_build(lv_obj_t *scr);
void hv_control_refresh(void);
void hv_control_key(hv_key_t key);

/* 编辑首页（home_edit.c） */
void hv_edit_key(hv_key_t key);
void hv_edit_decorate(void);              /* 重建页面后补编辑装饰 */
void hv_add_open(bool dock);
void hv_add_key(hv_key_t key);
void hv_add_close(void);

/* 语音卡片（home_voice.c） */
typedef enum {
    HV_VOICE_IDLE = 0,
    HV_VOICE_LISTENING,
    HV_VOICE_RECOGNIZING,
    HV_VOICE_THINKING,
    HV_VOICE_REPLY,
    HV_VOICE_ERROR,
} hv_voice_state_t;

void hv_voice_press(void);                /* 按下开始收音 */
void hv_voice_release(void);              /* 松开发送 */
void hv_voice_cancel(void);
void hv_voice_key(hv_key_t key);
void hv_voice_show(hv_voice_state_t st, const char *heard, const char *text);
hv_voice_state_t hv_voice_state(void);
const char *hv_voice_state_name(hv_voice_state_t st);
bool hv_voice_visible(void);
void hv_voice_hide(void);
const char *hv_voice_route_name(void);

/* 宠物（home_pet.c） */
lv_obj_t *hv_pet_create(lv_obj_t *parent, int32_t size);
void hv_pet_set_mood(lv_obj_t *pet, int mood);  /* 0 普通 1 开心 2 说话 */

/* 小工具 */
lv_obj_t *hv_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int32_t radius);
lv_obj_t *hv_text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);
void hv_plain(lv_obj_t *obj);             /* 去掉默认主题的边框/内边距/滚动 */
void hv_focus_ring(lv_obj_t *obj, bool on);
void hv_utf8_clip(char *s, size_t max_bytes);

#ifdef __cplusplus
}
#endif

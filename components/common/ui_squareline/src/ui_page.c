/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One carousel screen for every app page.
 *
 * SquareLine Studio generated seven screens (ui_Screen1/3/4/5/6/7/8) whose
 * chrome was byte-for-byte identical; they differed only in the icons, the
 * title, the accent colour, which indicator dot was highlighted, and which app
 * the middle button opened. Paging therefore meant destroying and rebuilding
 * ~640 lines of identical widgets, and every layout tweak had to be repeated
 * seven times.
 *
 * Here the chrome is built once and paging only re-binds the per-page data from
 * s_pages[]. Selecting an app is reported through a callback
 * (ui_squareline_page_selected) instead of being hard-coded into a screen, so
 * this component stays below the launcher in the dependency graph.
 */

#include "ui_page.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_icon_2048.h"
#include "ui_icon_app.h"
#include "ui_img_sd.h"
#include "ui_page_menu.h"
#include "ui_squareline_priv.h"

/* Duration of the slide animations defined in ui.c. The page is re-bound once
 * the slide has finished, which is what the old per-screen code achieved by
 * passing the same value as _ui_screen_change()'s delay. */
#define UI_PAGE_ANIM_MS 1000

/* Indicator dots: one per page below the selected icon, 22 px apart, centred on the screen. The
 * selected one is drawn larger and in the accent colour.
 *
 * The dot index *is* the page index, so the highlight always walks the row in
 * step with PREV/NEXT and a new page cannot get it out of sync. The generated
 * screens instead hard-coded a dot per screen with a rotation offset of 3
 * (Screen1 lit the fourth dot), which is why adding 2048 as an eighth page made
 * the highlight jump: it claimed the eighth dot while the seven older pages
 * still covered dots 0..6. */
#define UI_PAGE_DOT_SPACING     22
#define UI_PAGE_DOT_Y           (UI_CAROUSEL_MID_Y + 128)
#define UI_PAGE_DOT_SIZE        15
#define UI_PAGE_DOT_SIZE_SEL    18
#define UI_PAGE_DOT_COLOR       0x384038
#define UI_PAGE_DOT_COLOR_SEL   0xF0C430

/* Long-press sheets dim everything above the footer and rise from just above
 * it; the action sheet then sits right below the selected icon's frame. The
 * footer stays live as the key hint, with its middle caption switched to say
 * what M does while a sheet is up. Toasts appear between the dots and footer. */
#define UI_PAGE_SCREEN_H        800
#define UI_PAGE_FOOTER_H        56
#define UI_PAGE_SHEET_GAP       14
#define UI_PAGE_SHEET_BOTTOM    (UI_PAGE_SCREEN_H - UI_PAGE_FOOTER_H - UI_PAGE_SHEET_GAP)
#define UI_PAGE_SHEET_W         336
#define UI_PAGE_DETAIL_W        400     /* confirm and detail sheets */
#define UI_PAGE_TOAST_Y         640
#define UI_PAGE_OPEN_CAPTION    "M OPEN"
#define UI_PAGE_MENU_CAPTION    "M OK"

/* Installed apps: one accent for all of them, and room for their own strings. */
#define UI_PAGE_APP_ACCENT      0xF0C430
#define UI_PAGE_APP_TITLE_LEN   96
#define UI_PAGE_APP_ID_LEN      40
#define UI_PAGE_APP_LOCATION_LEN 128

/* Per-app data. Only the app's own appearance is listed here — the carousel
 * layout (which icon goes in which flanking slot) is derived at bind time from
 * the ring order, so adding, removing, or reordering pages requires no manual
 * neighbour bookkeeping.
 *
 * icon_sel   – image shown in the centre (selected) slot, usually a larger or
 *              higher-contrast version of the icon.
 * icon_unsel – image shown in the flanking (unselected) slots; also used as
 *              the in-flight source when a neighbour slides into the centre. */
typedef struct {
    const char *title;          /* big centred caption                           */
    const char *app_id;         /* stable id handed to the page-select callback  */
    const char *script;         /* default Lua script, or NULL                   */
    uint32_t    frame_color;    /* rounded frame around the selected icon        */
    uint32_t    pill_color;     /* small pill on top of that frame               */
    const char *icon_sel;       /* centre slot – selected state                  */
    const char *icon_unsel;     /* flanking slots – unselected state             */
    bool        renameable;
    bool        installed;      /* on the data partition, so it can be removed   */
    const char *location;       /* installed apps: where they live, for 详细      */
} ui_app_desc_t;

/* Maximum number of pages that can be registered at runtime.  The static table
 * below counts as the first batch; ui_page_register() fills the rest. */
#define UI_PAGE_MAX 48

/* Pages registered so far.  s_page_count is the live count; it grows as
 * ui_page_register() is called.  The first s_static_page_count entries are the
 * compile-time defaults and are never modified at runtime. */
static ui_app_desc_t s_pages[UI_PAGE_MAX];
static uint8_t       s_page_count       = 0;
static uint8_t       s_static_page_count = 0;

/* Copies of the installed apps' strings, indexed like s_pages: the owner's
 * buffers do not outlive ui_page_add_app(). */
typedef struct {
    char title[UI_PAGE_APP_TITLE_LEN];
    char app_id[UI_PAGE_APP_ID_LEN];
    char location[UI_PAGE_APP_LOCATION_LEN];
} ui_app_text_t;


#define PACKAGE_CACHE_SLOTS 5
static ui_squareline_app_provider_t s_provider;
static void *s_provider_ctx;
static uint32_t s_package_count;
static uint32_t s_provider_clock;
typedef struct {
    uint32_t page, used;
    ui_app_desc_t app;
    char title[96], id[40], location[256], selected[320], unselected[320];
} ui_package_slot_t;

/* ~18 KB of strings, heap-allocated on first use instead of living in .bss:
 * with SPIRAM_MALLOC_ALWAYSINTERNAL=0 malloc prefers PSRAM, which leaves the
 * internal RAM free for the BLE controller that starts later in boot. */
static ui_app_text_t *s_app_text;           /* UI_PAGE_MAX entries */
static ui_package_slot_t *s_package_cache;  /* PACKAGE_CACHE_SLOTS entries */
static bool ui_page_text_ready(void) {
    if (s_app_text == NULL) s_app_text = calloc(UI_PAGE_MAX, sizeof(*s_app_text));
    if (s_package_cache == NULL) s_package_cache = calloc(PACKAGE_CACHE_SLOTS, sizeof(*s_package_cache));
    return s_app_text != NULL && s_package_cache != NULL;
}
static uint32_t page_total(void) { return s_page_count + s_package_count; }
static uint32_t dot_count(void) { uint32_t n = page_total(); return n < 10 ? n : 10; }
void ui_page_set_provider(uint32_t count, ui_squareline_app_provider_t provider, void *ctx) {
    if (!ui_page_text_ready()) provider = NULL;
    s_provider = provider; s_provider_ctx = ctx; s_package_count = provider ? count : 0;
    if (s_package_cache != NULL) memset(s_package_cache, 0, PACKAGE_CACHE_SLOTS * sizeof(*s_package_cache));
    s_provider_clock = 0;
}
static const ui_app_desc_t *page_at(uint32_t page) {
    if (page < s_page_count || s_package_cache == NULL) return &s_pages[page < s_page_count ? page : 0];
    uint32_t slot = 0;
    for (uint32_t i = 0; i < PACKAGE_CACHE_SLOTS; i++) {
        if (s_package_cache[i].used && s_package_cache[i].page == page) { s_package_cache[i].used = ++s_provider_clock; return &s_package_cache[i].app; }
        if (s_package_cache[i].used < s_package_cache[slot].used) slot = i;
    }
    ui_squareline_app_t app = {0};
    bool ok = s_provider && s_provider(page - s_page_count, &app, s_provider_ctx);
    strlcpy(s_package_cache[slot].title, ok && app.title ? app.title : "应用不可用", sizeof(s_package_cache[slot].title));
    strlcpy(s_package_cache[slot].id, ok && app.app_id ? app.app_id : "", sizeof(s_package_cache[slot].id));
    strlcpy(s_package_cache[slot].location, ok && app.location ? app.location : "", sizeof(s_package_cache[slot].location));
    strlcpy(s_package_cache[slot].selected, ok && app.icon_selected ? app.icon_selected : UI_ICON_APP_NAME, sizeof(s_package_cache[slot].selected));
    strlcpy(s_package_cache[slot].unselected, ok && app.icon_unselected ? app.icon_unselected : UI_ICON_APP_NAME, sizeof(s_package_cache[slot].unselected));
    s_package_cache[slot].app = (ui_app_desc_t){
        .title = s_package_cache[slot].title, .app_id = s_package_cache[slot].id,
        .location = s_package_cache[slot].location, .icon_sel = s_package_cache[slot].selected,
        .icon_unsel = s_package_cache[slot].unselected,
        .frame_color = ok ? app.frame_color : UI_PAGE_APP_ACCENT, .pill_color = ok ? app.pill_color : UI_PAGE_APP_ACCENT,
        .installed = ok, .renameable = ok && app.renameable,
    };
    s_package_cache[slot].page = page; s_package_cache[slot].used = ++s_provider_clock;
    return &s_package_cache[slot].app;
}

/* Ring accessor: index modulo live count. */
static inline uint32_t page_ring(int64_t idx)
{
    int64_t n = (int64_t)page_total();
    return (uint32_t)(((idx % n) + n) % n);
}

/* Compile-time page table.  Every app lists only its own two icons; the carousel
 * figures out which neighbour goes in which slot from the ring order at bind
 * time.  To add, remove, or reorder pages edit only this array — nothing else.
 *
 * The unselected icons are the ones the generated screens used for this app
 * whenever it sat in a flanking slot; both neighbours that ever showed it agreed
 * on the value, so the ring is self-consistent. */
static const ui_app_desc_t s_default_pages[] = {
    {
        .title = "Snake Game",   .app_id = "local_apps_1",
        .script = "skills/snake_game/scripts/snake_game.lua",
        .frame_color = 0xFCC430, .pill_color = 0xF0C430,
        .icon_sel   = "ui_img_570872286",
        .icon_unsel = "ui_img_1112462947",
    },
    {
        .title = "Pomodoro Timer", .app_id = "local_apps_3", .script = NULL,
        .frame_color = 0xC7EF38, .pill_color = 0xC7EF38,
        .icon_sel   = "ui_img_329034005",
        .icon_unsel = "ui_img_1238136554",
    },
    {
        .title = "Offline Clock", .app_id = "local_apps_4", .script = NULL,
        .frame_color = 0xC9E75D, .pill_color = 0xC9E75D,
        .icon_sel   = "ui_img_1995884468",
        .icon_unsel = "ui_img_1431988785",
    },
    {
        .title = "Conference tools", .app_id = "meeting_presenter",
        .script = "skills/ble_hid/scripts/meeting_presenter.lua",
        .frame_color = 0xFFE45B, .pill_color = 0xFFE45B,
        .icon_sel   = "ui_img_52855490",
        .icon_unsel = "ui_img_503997393",
    },
    {
        .title = "Meeting Minutes", .app_id = "local_apps_6", .script = NULL,
        .frame_color = 0xFFD800, .pill_color = 0xFFD800,
        .icon_sel   = "ui_img_1482846779",
        .icon_unsel = "ui_img_631048702",
    },
    {
        .title = "Infrared remote control", .app_id = "local_apps_7", .script = NULL,
        .frame_color = 0xFCAE15, .pill_color = 0xFCAE15,
        .icon_sel   = "ui_img_1147949500",
        .icon_unsel = "ui_img_531009829",
    },
    {
        .title = "Business card sharing", .app_id = "local_apps_8", .script = NULL,
        .frame_color = 0xD4EC56, .pill_color = 0xD4EC56,
        .icon_sel   = "ui_img_752904243",
        .icon_unsel = "ui_img_467453088",
    },
    {
        .title = "GAME  2048", .app_id = "game_2048",
        .script = "skills/game_2048/scripts/game_2048.lua",
        .frame_color = 0xEDC22E, .pill_color = 0xEDC22E,
        .icon_sel   = UI_ICON_2048_NAME,
        .icon_unsel = UI_ICON_2048_NAME,
    },
    {
        .title = "WorkBuddy", .app_id = "workbuddy", .script = NULL,
        .frame_color = 0xF0DC18, .pill_color = 0xF0DC18,
        .icon_sel   = "workbuddy_select",
        .icon_unsel = "workbuddy_unselect",
    },
};

#define UI_PAGE_COUNT ((uint8_t)(sizeof(s_default_pages) / sizeof(s_default_pages[0])))

/* Widgets whose content depends on the current page. Everything else is created
 * once and never touched again, so it needs no handle. */
static struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *frame;
    lv_obj_t *pill;
    lv_obj_t *icon_mid;
    lv_obj_t *icon_right;
    lv_obj_t *icon_left;
    lv_obj_t *icon_right_in;
    lv_obj_t *icon_left_in;
    lv_obj_t *footer_open;      /* middle caption, relabelled while the menu is up */
    lv_obj_t **dots;            /* allocated in screen_init, length = s_page_count */
    lv_timer_t *settle;         /* fires when a slide finishes, then re-binds */
    uint32_t  page;
    uint32_t  pending;          /* page to bind when s_ui.settle fires */
} s_ui;

/* ------------------------------------------------------------------ helpers */

/* Seed the runtime page table with the compile-time defaults, once. */
static void ui_page_load_defaults(void)
{
    if (s_page_count != 0) {
        return;
    }
    s_static_page_count = (uint8_t)(sizeof(s_default_pages) / sizeof(s_default_pages[0]));
    for (uint8_t i = 0; i < s_static_page_count && i < UI_PAGE_MAX; i++) {
        s_pages[i] = s_default_pages[i];
    }
    s_page_count = s_static_page_count;
}

/* Text areas come in two flavours in the original screens: captions that use
 * the custom font, and the two status strings that keep the default font and a
 * placeholder. Both share the transparent-background / no-border styling. */
static lv_obj_t *ui_page_label(lv_obj_t *parent, int w, int h, int x, int y,
                               const char *text, uint32_t color, bool custom_font)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, w);
    lv_obj_set_height(ta, h);
    lv_obj_set_x(ta, x);
    lv_obj_set_y(ta, y);
    lv_obj_set_align(ta, LV_ALIGN_CENTER);
    lv_textarea_set_text(ta, text);
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(ta, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ta, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x080C08), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (custom_font) {
        lv_obj_set_style_text_font(ta, &ui_font_Font1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ta, &ui_font_Font1, LV_PART_SELECTED | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ta, lv_color_hex(0x598078), LV_PART_CURSOR | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ta, 255, LV_PART_CURSOR | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ta, &ui_font_Font1, LV_PART_CURSOR | LV_STATE_DEFAULT);
    } else {
        lv_textarea_set_placeholder_text(ta, "Placeholder...");
    }
    return ta;
}

/* Plain rectangle/round block: the status bars, the frame and the dots. */
static lv_obj_t *ui_page_block(lv_obj_t *parent, int w, int h, int x, int y, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, w);
    lv_obj_set_height(obj, h);
    lv_obj_set_x(obj, x);
    lv_obj_set_y(obj, y);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (radius) {
        lv_obj_set_style_radius(obj, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    return obj;
}

static void ui_page_block_fill(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* Rounded frame around the selected icon, with the pill on its top edge. Used by
 * the carousel and by the copy the long-press menu lifts above its scrim. */
static lv_obj_t *ui_page_frame(lv_obj_t *parent, uint32_t frame_color, uint32_t pill_color,
                               lv_obj_t **ret_pill)
{
    lv_obj_t *frame = ui_page_block(parent, UI_CAROUSEL_FRAME_W, UI_CAROUSEL_FRAME_H,
                                    0, UI_CAROUSEL_FRAME_Y, UI_CAROUSEL_FRAME_RADIUS);
    lv_obj_t *pill;

    lv_obj_set_style_border_color(frame, lv_color_hex(frame_color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(frame, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(frame, UI_CAROUSEL_FRAME_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);

    pill = ui_page_block(frame, UI_CAROUSEL_PILL_W, UI_CAROUSEL_PILL_H,
                         0, UI_CAROUSEL_PILL_Y, UI_CAROUSEL_PILL_RADIUS);
    ui_page_block_fill(pill, pill_color);
    if (ret_pill != NULL) {
        *ret_pill = pill;
    }
    return frame;
}

/* Resolve a page-table icon name to something lv_image_set_src() accepts.
 *
 * Almost every name refers to a pre-rendered .bin on the SD card. The 2048 page
 * has no artwork file and names the sentinel instead, which resolves to a buffer
 * drawn with LVGL. Both are valid image sources, so the carousel does not care
 * which one it gets.
 *
 * @p selected picks the icon variant: the centre slot is drawn at full contrast,
 * the flanking slots recede into the backdrop. For SD-card names the flag is
 * unused - those files carry a single appearance and the carousel differentiates
 * them by scale and by the frame behind the centre slot. */
static const void *ui_page_icon_src(lv_obj_t *parent, const char *name, bool selected)
{
    if (name == NULL) {
        return NULL;
    }
    if (strcmp(name, UI_ICON_2048_NAME) == 0) {
        return ui_icon_2048_src(parent, selected);
    }
    if (strcmp(name, UI_ICON_APP_NAME) == 0) {
        return ui_icon_app_src(parent, selected);
    }
    if (strncmp(name, "S:/", 3) == 0) return name;
    return ui_img_sd_path(name);
}

/* Carousel icon. The two hidden in-flight slots start fully transparent. */
static lv_obj_t *ui_page_icon(lv_obj_t *parent, const char *name,
                              int x, int y, int zoom, bool hidden, bool selected)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, ui_page_icon_src(parent, name, selected));
    lv_obj_set_width(img, LV_SIZE_CONTENT);
    lv_obj_set_height(img, LV_SIZE_CONTENT);
    lv_obj_set_x(img, x);
    lv_obj_set_y(img, y);
    lv_obj_set_align(img, LV_ALIGN_CENTER);
    lv_obj_add_flag(img, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_img_set_zoom(img, zoom);
    if (hidden) {
        lv_obj_set_style_blend_mode(img, LV_BLEND_MODE_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    return img;
}

/* Transparent hit target sitting on top of one of the bottom captions. */
static lv_obj_t *ui_page_button(lv_obj_t *parent, int w, int h, int x, int y, bool outline)
{
    (void)outline;
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, w);
    lv_obj_set_height(btn, h);
    lv_obj_set_x(btn, x);
    lv_obj_set_y(btn, y);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_border_opa(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_outline_opa(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_set_style_shadow_opa(btn, 0, LV_PART_MAIN | LV_STATE_ANY);
    return btn;
}

/* --------------------------------------------------------------- page bind */

/* Apply a page's data to the shared widgets and restore the designed carousel
 * layout. The slide animations write real object properties, so their end state
 * has to be undone before the next page is shown. */
static void ui_page_bind(uint32_t page)
{
    lv_obj_t *slots[5];

    if (s_ui.screen == NULL || page >= page_total()) {
        return;
    }
    s_ui.page = page;

    /* Derive the five carousel slots from the ring without any manual neighbour
     * bookkeeping.  Given the current page index:
     *   mid      = pages[cur].icon_sel    — selected, large centre slot
     *   right    = pages[cur+1].icon_unsel — right neighbour (next page)
     *   left     = pages[cur-1].icon_unsel — left neighbour (previous page)
     *   right_in = pages[cur+2].icon_unsel — source the next page flies in from
     *   left_in  = pages[cur-2].icon_unsel — source the prev page flies in from
     */
    const ui_app_desc_t *cur  = page_at(page);
    const ui_app_desc_t *next = page_at(page_ring((int64_t)page + 1));
    const ui_app_desc_t *prev = page_at(page_ring((int64_t)page - 1));
    const ui_app_desc_t *next2 = page_at(page_ring((int64_t)page + 2));
    const ui_app_desc_t *prev2 = page_at(page_ring((int64_t)page - 2));

    slots[0] = s_ui.icon_mid;
    slots[1] = s_ui.icon_right;
    slots[2] = s_ui.icon_left;
    slots[3] = s_ui.icon_right_in;
    slots[4] = s_ui.icon_left_in;

    /* Drop any still-running slide so a late frame cannot overwrite the reset
     * below with the previous page's end values. */
    for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        lv_anim_del(slots[i], NULL);
    }

    lv_img_set_src(s_ui.icon_mid,      ui_page_icon_src(s_ui.screen, cur->icon_sel,    true));
    lv_img_set_src(s_ui.icon_right,    ui_page_icon_src(s_ui.screen, next->icon_unsel, false));
    lv_img_set_src(s_ui.icon_left,     ui_page_icon_src(s_ui.screen, prev->icon_unsel, false));
    lv_img_set_src(s_ui.icon_right_in, ui_page_icon_src(s_ui.screen, next2->icon_unsel, false));
    lv_img_set_src(s_ui.icon_left_in,  ui_page_icon_src(s_ui.screen, prev2->icon_unsel, false));

    _ui_carousel_reset(s_ui.icon_mid, s_ui.icon_right, s_ui.icon_left,
                       s_ui.icon_right_in, s_ui.icon_left_in);

    lv_textarea_set_text(s_ui.title, cur->title);
    /* Installed apps may be named in Chinese, which Font1 lacks. */
    lv_obj_set_style_text_font(s_ui.title,
                               cur->installed && ui_page_menu_get_font() != NULL
                               ? ui_page_menu_get_font() : &ui_font_Font1,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_ui.frame, lv_color_hex(cur->frame_color),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_page_block_fill(s_ui.pill, cur->pill_color);

    for (uint32_t i = 0; i < dot_count(); i++) {
        bool sel = (i == page % 10);
        if (page / 10 * 10 + i < page_total()) lv_obj_remove_flag(s_ui.dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.dots[i], LV_OBJ_FLAG_HIDDEN);
        int size = sel ? UI_PAGE_DOT_SIZE_SEL : UI_PAGE_DOT_SIZE;
        lv_obj_set_width(s_ui.dots[i], size);
        lv_obj_set_height(s_ui.dots[i], size);
        ui_page_block_fill(s_ui.dots[i], sel ? UI_PAGE_DOT_COLOR_SEL : UI_PAGE_DOT_COLOR);
    }
}

/* ------------------------------------------------------------------- events */

static void ui_page_settle_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    s_ui.settle = NULL;       /* repeat count 1: LVGL deletes it after this call */
    ui_page_bind(s_ui.pending);
}

/* Start the slide towards @p target and re-bind once it has played out. While a
 * slide is in flight further presses are ignored, which the old screen-swapping
 * code could not do. */
static void ui_page_slide(uint32_t target, bool forward)
{
    if (s_ui.screen == NULL || s_ui.settle != NULL) {
        return;
    }

    if (forward) {
        switchtoP1leftpre_Animation(s_ui.icon_left, 0);
        switchtoP1midpre_Animation(s_ui.icon_mid, 0);
        switchtoP1rightinpre_Animation(s_ui.icon_right_in, 0);
        switchtoP1rightpre_Animation(s_ui.icon_right, 0);
    } else {
        P0switchtoP6right_Animation(s_ui.icon_right, 0);
        switchtoP6mid_Animation(s_ui.icon_mid, 0);
        P0switchtoP6left_Animation(s_ui.icon_left, 0);
        P0switchtoP6leftin_Animation(s_ui.icon_left_in, 0);
    }

    s_ui.pending = target;
    s_ui.settle = lv_timer_create(ui_page_settle_cb, UI_PAGE_ANIM_MS, NULL);
    if (s_ui.settle != NULL) {
        lv_timer_set_repeat_count(s_ui.settle, 1);
    } else {
        ui_page_bind(target);   /* no timer available: bind straight away */
    }
}

static lv_obj_t *s_rename_overlay, *s_rename_input;
static void ui_page_rename_finish(bool save)
{
    if (!s_rename_overlay) return;
    if (save) {
        const char *text = lv_textarea_get_text(s_rename_input);
        if (!text[0] || strlen(text) > 95 || strspn(text, " ") == strlen(text)) {
            ui_page_menu_toast(s_ui.screen, UI_PAGE_TOAST_Y, "名称不能为空，最多95字节"); return;
        }
        ui_squareline_request_app_rename(s_ui.page, page_at(s_ui.page)->app_id, text);
    }
    lv_obj_delete(s_rename_overlay); s_rename_overlay = s_rename_input = NULL;
    lv_textarea_set_text(s_ui.footer_open, UI_PAGE_OPEN_CAPTION);
}
static void ui_page_rename_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) ui_page_rename_finish(true);
    else if (lv_event_get_code(e) == LV_EVENT_CANCEL) ui_page_rename_finish(false);
}
static void ui_page_rename_button(lv_event_t *e)
{
    ui_page_rename_finish((uintptr_t)lv_event_get_user_data(e) != 0);
}
static void ui_page_rename_open(void)
{
    if (s_rename_overlay) return;
    s_rename_overlay = lv_obj_create(s_ui.screen);
    lv_obj_set_size(s_rename_overlay, 480, 744); lv_obj_set_pos(s_rename_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_rename_overlay, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_width(s_rename_overlay, 0, 0);
    lv_obj_t *heading = lv_label_create(s_rename_overlay);
    lv_obj_set_style_text_color(heading, lv_color_hex(0xFAFBF5), 0);
    lv_label_set_text(heading, "重命名应用"); lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 90);
    if (ui_page_menu_get_font()) lv_obj_set_style_text_font(s_rename_overlay, ui_page_menu_get_font(), 0);
    s_rename_input = lv_textarea_create(s_rename_overlay);
    lv_textarea_set_one_line(s_rename_input, true); lv_textarea_set_max_length(s_rename_input, 95);
    lv_obj_set_size(s_rename_input, 410, 72); lv_obj_align(s_rename_input, LV_ALIGN_TOP_MID, 0, 160);
    lv_textarea_set_text(s_rename_input, page_at(s_ui.page)->title);
    lv_obj_t *hint = lv_label_create(s_rename_overlay);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xBFC7C0), 0);
    lv_label_set_text(hint, "确认保存 / 取消返回\n中文名称也可从电脑端修改");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 260);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *button = lv_button_create(s_rename_overlay);
        lv_obj_set_size(button, 175, 50); lv_obj_align(button, LV_ALIGN_TOP_MID, i ? 105 : -105, 340);
        lv_obj_set_style_bg_color(button, lv_color_hex(i ? 0x23866D : 0x354039), 0);
        lv_obj_t *label = lv_label_create(button); lv_label_set_text(label, i ? "保存" : "取消");
        lv_obj_center(label); lv_obj_set_style_text_color(label, lv_color_hex(0xFAFBF5), 0);
        lv_obj_add_event_cb(button, ui_page_rename_button, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    lv_obj_t *keyboard = lv_keyboard_create(s_rename_overlay);
    lv_obj_set_size(keyboard, 448, 290); lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, s_rename_input);
    lv_obj_add_event_cb(keyboard, ui_page_rename_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, ui_page_rename_event, LV_EVENT_CANCEL, NULL);
    lv_textarea_set_text(s_ui.footer_open, UI_PAGE_MENU_CAPTION);
}

/* The three user actions the screen offers. Both the on-screen hit targets and
 * the board's physical keys funnel through these, so a key press behaves exactly
 * like a tap: same slide animation, same settle timer, same select dispatch.
 *
 * The left-hand "PREV" caption advanced to the next screen in the generated
 * chain and the right-hand "NEXT" one went back; those bindings are kept as they
 * were, so the ring index moves opposite to the captions. */
void ui_page_prev(void)
{
    if (s_rename_overlay) return;
    if (ui_page_menu_is_open()) {
        ui_page_menu_step(-1);
        return;
    }
    ui_page_slide(page_ring((int64_t)s_ui.page - 1), false);
}

void ui_page_next(void)
{
    if (s_rename_overlay) return;
    if (ui_page_menu_is_open()) {
        ui_page_menu_step(1);
        return;
    }
    ui_page_slide(page_ring((int64_t)s_ui.page + 1), true);
}

/* "M OPEN": report the selection and let the owner decide what to open. */
void ui_page_select(void)
{
    if (s_rename_overlay) { ui_page_rename_finish(true); return; }
    const ui_app_desc_t *p;

    if (s_ui.screen == NULL) {
        return;
    }
    if (ui_page_menu_is_open()) {
        ui_page_menu_confirm();
        return;
    }
    p = page_at(s_ui.page);
    ui_squareline_page_selected(s_ui.page, p->app_id, p->script);
}

static void ui_page_left_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_page_prev();
    }
}

static void ui_page_right_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_page_next();
    }
}

/* Touch swipe handling for page navigation */
#define SWIPE_THRESHOLD 44

static struct {
    bool is_pressed;
    int32_t start_x;
    int32_t start_y;
} page_touch_state = { false, 0, 0 };

static void ui_page_touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (!indev) return;

    /* The footer stays live under the menu; a swipe there must not page. */
    if (ui_page_menu_is_open()) {
        page_touch_state.is_pressed = false;
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        page_touch_state.is_pressed = true;
        page_touch_state.start_x = point.x;
        page_touch_state.start_y = point.y;
    }
    else if (code == LV_EVENT_RELEASED) {
        if (page_touch_state.is_pressed) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            int32_t dx = point.x - page_touch_state.start_x;
            int32_t dy = point.y - page_touch_state.start_y;

            /* Check if this is a horizontal swipe */
            if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                if (dx > 0) {
                    /* Swipe right -> previous page */
                    ui_page_prev();
                } else {
                    /* Swipe left -> next page */
                    ui_page_next();
                }
            }

            page_touch_state.is_pressed = false;
        }
    }
}

static void ui_page_open_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_page_select();
    }
}

static void ui_page_mid_icon_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DOUBLE_CLICKED) {
        ui_page_select();
    }
}

/* ---------------------------------------------------------- long-press flow */

/* Long press on the selected icon opens the action sheet. 卸载 refuses built-in
 * apps with a toast and asks before removing an installed one; 详细 swaps in an
 * information sheet. Only a confirmed uninstall leaves this file, through
 * ui_squareline_page_action(): the owner deletes the app and rebuilds the home
 * screen. */
typedef enum {
    UI_PAGE_SHEET_ACTIONS = 0,
    UI_PAGE_SHEET_CONFIRM,
    UI_PAGE_SHEET_DETAIL,
} ui_page_sheet_t;

enum { ACTION_UNINSTALL, ACTION_RENAME, ACTION_DETAIL, ACTION_CANCEL, ACTION_COUNT };
enum { CONFIRM_UNINSTALL, CONFIRM_CANCEL, CONFIRM_COUNT };

static const ui_page_menu_row_t s_action_rows[ACTION_COUNT] = {
    [ACTION_UNINSTALL] = { "卸载", UI_PAGE_MENU_ICON_TRASH, true  },
    [ACTION_RENAME]    = { "重命名", UI_PAGE_MENU_ICON_INFO, false },
    [ACTION_DETAIL]    = { "详细", UI_PAGE_MENU_ICON_INFO,  false },
    [ACTION_CANCEL]    = { "取消", UI_PAGE_MENU_ICON_CLOSE, false },
};

static const ui_page_menu_row_t s_confirm_rows[CONFIRM_COUNT] = {
    [CONFIRM_UNINSTALL] = { "卸载", UI_PAGE_MENU_ICON_TRASH, true  },
    [CONFIRM_CANCEL]    = { "取消", UI_PAGE_MENU_ICON_CLOSE, false },
};

static const ui_page_menu_row_t s_detail_rows[] = {
    { "关闭", UI_PAGE_MENU_ICON_CLOSE, false },
};

static ui_page_sheet_t s_sheet;
static char s_sheet_heading[UI_PAGE_APP_TITLE_LEN + 16];

static void ui_page_sheet_done(uint8_t row);

static void ui_page_toast(const char *text)
{
    if (s_ui.screen != NULL) {
        ui_page_menu_toast(s_ui.screen, UI_PAGE_TOAST_Y, text);
    }
}

/* Open @p config as @p sheet. With @p lift, a copy of the framed icon is raised
 * above the scrim so the user can see which app the sheet is about; sheets tall
 * enough to cover the frame name the app in their heading instead. */
static void ui_page_sheet_open(ui_page_sheet_t sheet, ui_page_menu_config_t *config, bool lift)
{
    const ui_app_desc_t *p = page_at(s_ui.page);
    lv_obj_t *layer;
    lv_obj_t *icon;

    config->parent = s_ui.screen;
    config->scrim_h = UI_PAGE_SCREEN_H - UI_PAGE_FOOTER_H;
    config->panel_bottom = UI_PAGE_SHEET_BOTTOM;
    config->done_cb = ui_page_sheet_done;
    layer = ui_page_menu_open(config);
    if (layer == NULL) {
        return;
    }
    s_sheet = sheet;
    if (lift) {
        ui_page_frame(layer, p->frame_color, p->pill_color, NULL);
        icon = ui_page_icon(layer, NULL, 0, UI_CAROUSEL_MID_Y, UI_CAROUSEL_MID_ZOOM, false, true);
        lv_img_set_src(icon, lv_img_get_src(s_ui.icon_mid));
    }
    lv_textarea_set_text(s_ui.footer_open, UI_PAGE_MENU_CAPTION);
}

static void ui_page_actions_open(void)
{
    ui_page_menu_config_t config = {
        .panel_w    = UI_PAGE_SHEET_W,
        .heading    = page_at(s_ui.page)->title,
        .rows       = s_action_rows,
        .row_count  = ACTION_COUNT,
        .focus      = ACTION_CANCEL,
        .cancel_row = ACTION_CANCEL,
    };

    ui_page_sheet_open(UI_PAGE_SHEET_ACTIONS, &config, false);
}

static void ui_page_confirm_open(void)
{
    ui_page_menu_config_t config = {
        .panel_w    = UI_PAGE_DETAIL_W,
        .heading    = s_sheet_heading,
        .message    = "应用文件将被删除，无法恢复",
        .rows       = s_confirm_rows,
        .row_count  = CONFIRM_COUNT,
        .focus      = CONFIRM_CANCEL,
        .cancel_row = CONFIRM_CANCEL,
    };

    snprintf(s_sheet_heading, sizeof(s_sheet_heading), "卸载「%s」？", page_at(s_ui.page)->title);
    ui_page_sheet_open(UI_PAGE_SHEET_CONFIRM, &config, false);
}

static void ui_page_detail_open(void)
{
    const ui_app_desc_t *p = page_at(s_ui.page);
    const ui_page_menu_field_t fields[] = {
        { "类型", p->installed ? "已安装应用" : "内置应用" },
        { "应用 ID", p->app_id != NULL ? p->app_id : "-" },
        p->installed ? (ui_page_menu_field_t){ "位置", p->location != NULL ? p->location : "-" }
                     : (ui_page_menu_field_t){ "入口", p->script != NULL ? p->script : "固件界面" },
    };
    ui_page_menu_config_t config = {
        .panel_w     = UI_PAGE_DETAIL_W,
        .heading     = p->title,
        .fields      = fields,
        .field_count = (uint8_t)(sizeof(fields) / sizeof(fields[0])),
        .rows        = s_detail_rows,
        .row_count   = 1,
    };

    ui_page_sheet_open(UI_PAGE_SHEET_DETAIL, &config, false);
}

static void ui_page_sheet_done(uint8_t row)
{
    const ui_app_desc_t *p = page_at(s_ui.page);

    lv_textarea_set_text(s_ui.footer_open, UI_PAGE_OPEN_CAPTION);
    switch (s_sheet) {
    case UI_PAGE_SHEET_ACTIONS:
        if (row == ACTION_UNINSTALL) {
            if (p->installed) {
                ui_page_confirm_open();
            } else {
                ui_page_toast("系统应用不可卸载");
            }
        } else if (row == ACTION_RENAME) {
            if (p->renameable) ui_page_rename_open();
            else ui_page_toast(p->installed ? "旧格式应用请从原上位机修改" : "系统应用不可重命名");
        } else if (row == ACTION_DETAIL) {
            ui_page_detail_open();
        }
        break;
    case UI_PAGE_SHEET_CONFIRM:
        if (row == CONFIRM_UNINSTALL) {
            ui_page_toast("正在卸载…");
            ui_squareline_page_action(s_ui.page, p->app_id, UI_SQUARELINE_PAGE_ACTION_UNINSTALL);
        }
        break;
    case UI_PAGE_SHEET_DETAIL:
    default:
        break;
    }
}

static void ui_page_mid_icon_long_cb(lv_event_t *e)
{
    if (s_rename_overlay || lv_event_get_code(e) != LV_EVENT_LONG_PRESSED || s_ui.settle != NULL) {
        return;
    }
    ui_page_actions_open();
}

/* ------------------------------------------------------------------- screen */

void ui_page_screen_init(void)
{
    lv_obj_t *root;
    lv_obj_t *bar;
    lv_obj_t *obj;
    lv_obj_t *btn;
    lv_obj_t *footer_prev;
    lv_obj_t *footer_open;
    lv_obj_t *footer_next;

    if (s_ui.screen != NULL) {
        return;
    }

    /* ui_page_register() / ui_page_add_app() may have been called before
     * screen_init to append extra pages; those sit after the static ones. */
    ui_page_load_defaults();

    /* Allocate the dot-widget pointer array now that we know the live count. */
    s_ui.dots = lv_malloc(dot_count() * sizeof(lv_obj_t *));
    if (s_ui.dots == NULL) {
        return;
    }

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(s_ui.screen, lv_color_hex(0x798078), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s_ui.screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(s_ui.screen, lv_color_hex(0x000000), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(s_ui.screen, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);

    /* Black backdrop that owns every other widget. */
    root = ui_page_block(s_ui.screen, 480, 800, 0, 0, 0);
    ui_page_block_fill(root, 0x000000);
    lv_obj_set_style_bg_img_recolor(root, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(root, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(root, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(root, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_color(root, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_opa(root, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(root, lv_color_hex(0xF0DC18), LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(root, 255, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);

    /* Status bar with its yellow marker. */
    bar = ui_page_block(root, 480, 48, 0, -376, 0);
    ui_page_block_fill(bar, 0x080C08);
    ui_page_block_fill(ui_page_block(bar, 12, 48, -234, 0, 0), 0xF8E458);

    ui_page_label(root, 88, 54, 140, -366, "9:51", 0xFAFBF5, false);

    /* Battery gauge. */
    obj = lv_bar_create(root);
    lv_bar_set_value(obj, 70, LV_ANIM_OFF);
    lv_bar_set_start_value(obj, 0, LV_ANIM_OFF);
    lv_obj_set_width(obj, 32);
    lv_obj_set_height(obj, 9);
    lv_obj_set_x(obj, 208);
    lv_obj_set_y(obj, -376);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x384038), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF0DC18), LV_PART_MAIN | LV_STATE_SCROLLED);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_SCROLLED);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0xF0DC18), LV_PART_MAIN | LV_STATE_SCROLLED);
    lv_obj_set_style_radius(obj, 1, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF0DC18), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0xF0DC18), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    /* Header strip with the orange status dot. */
    bar = ui_page_block(root, 240, 41, 0, -296, 0);
    ui_page_block_fill(bar, 0x080C08);
    obj = ui_page_block(bar, 17, 16, -88, 2, 75);
    ui_page_block_fill(obj, 0xF8AC10);

    ui_page_label(root, 93, 54, 1, -289, "会议纪要", 0xFAFBF5, true);

    /* Footer strip: top border only. */
    bar = ui_page_block(root, 480, 56, 0, 372, 0);
    ui_page_block_fill(bar, 0x080C08);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x384038), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_pad(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    footer_prev = ui_page_label(root, 93, 45, -150, 378, "PREV", 0xF0C428, true);
    footer_open = ui_page_label(root, 93, 49, -1, 378, UI_PAGE_OPEN_CAPTION, 0xF0C428, true);
    s_ui.footer_open = footer_open;

    /* Page caption — rebound on every page change; empty string until ui_page_bind(0) below. */
    s_ui.title = ui_page_label(root, 360, 45, 4, UI_CAROUSEL_MID_Y - 145, "", 0x788078, true);
    lv_textarea_set_one_line(s_ui.title, true);
    lv_obj_set_style_text_align(s_ui.title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_ui.title, lv_color_hex(0x786028), LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_page_label(root, 135, 58, -153, -367, "LANGUAGE", 0xFAFBF5, false);

    /* Frame + pill highlighting the selected icon. Colours rebound by ui_page_bind(0). */
    s_ui.frame = ui_page_frame(root, 0x000000, 0x000000, &s_ui.pill);

    /* Visible carousel slots — sources will be overwritten by ui_page_bind(0)
     * below; passing NULL here is safe because set_src accepts NULL. */
    s_ui.icon_mid   = ui_page_icon(root, NULL, 0, UI_CAROUSEL_MID_Y,
                                   UI_CAROUSEL_MID_ZOOM, false, true);
    s_ui.icon_right = ui_page_icon(root, NULL, UI_CAROUSEL_SIDE_X, UI_CAROUSEL_SIDE_Y,
                                   UI_CAROUSEL_SIDE_ZOOM, false, false);
    s_ui.icon_left  = ui_page_icon(root, NULL, -UI_CAROUSEL_SIDE_X, UI_CAROUSEL_SIDE_Y,
                                   UI_CAROUSEL_SIDE_ZOOM, false, false);

    /* Indicator dots, left to right. */
    for (uint32_t i = 0; i < dot_count(); i++) {
        int x = ((int)i * 2 - (int)(dot_count() - 1)) * UI_PAGE_DOT_SPACING / 2;
        s_ui.dots[i] = ui_page_block(root, UI_PAGE_DOT_SIZE, UI_PAGE_DOT_SIZE,
                                     x, UI_PAGE_DOT_Y, 75);
        ui_page_block_fill(s_ui.dots[i], UI_PAGE_DOT_COLOR);
    }

    /* Static arrow glyphs beside the footer buttons. */
    ui_page_icon(root, "ui_img_1793379158",  212, 373, 64, false, false);
    ui_page_icon(root, "ui_img_1111249434", -212, 374, 64, false, false);

    /* Hidden slots the incoming neighbour flies in from.
     * Sources are overwritten by ui_page_bind(0) below. */
    s_ui.icon_right_in = ui_page_icon(root, NULL, 0, UI_CAROUSEL_MID_Y,
                                      UI_CAROUSEL_SIDE_ZOOM, true, false);
    lv_obj_add_state(s_ui.icon_right_in, LV_STATE_USER_1);
    s_ui.icon_left_in  = ui_page_icon(root, NULL, 0, UI_CAROUSEL_MID_Y,
                                      UI_CAROUSEL_SIDE_ZOOM, true, false);

    footer_next = ui_page_label(root, 93, 66, 154, 387, "NEXT", 0xF0C428, true);
    lv_obj_set_flex_flow(footer_next, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer_next, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    btn = ui_page_button(root, 120, 56, 170, 372, true);
    lv_obj_add_event_cb(btn, ui_page_right_cb, LV_EVENT_ALL, NULL);

    btn = ui_page_button(root, 120, 56, -170, 372, false);
    lv_obj_add_event_cb(btn, ui_page_left_cb, LV_EVENT_ALL, NULL);

    btn = ui_page_button(root, 120, 56, 0, 372, true);
    lv_obj_add_event_cb(btn, ui_page_open_cb, LV_EVENT_ALL, NULL);

    lv_obj_clear_flag(footer_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(footer_open, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(footer_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(footer_prev);
    lv_obj_move_foreground(footer_open);
    lv_obj_move_foreground(footer_next);

    lv_obj_add_flag(s_ui.icon_mid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_ui.icon_mid, ui_page_mid_icon_cb, LV_EVENT_DOUBLE_CLICKED, NULL);
    lv_obj_add_event_cb(s_ui.icon_mid, ui_page_mid_icon_long_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* Register touch events for swipe navigation */
    lv_obj_add_event_cb(s_ui.screen, ui_page_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.screen, ui_page_touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, ui_page_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, ui_page_touch_event_cb, LV_EVENT_RELEASED, NULL);

    ui_page_bind(0);
}

void ui_page_screen_destroy(void)
{
    ui_page_rename_finish(false);
    ui_page_menu_close();
    ui_page_menu_toast_clear();
    if (s_ui.settle != NULL) {
        lv_timer_del(s_ui.settle);
    }
    if (s_ui.screen != NULL) {
        lv_obj_del(s_ui.screen);
    }
    if (s_ui.dots != NULL) {
        lv_free(s_ui.dots);
    }
    /* After the slots are gone, so nothing still points at the buffers. */
    ui_icon_2048_release();
    ui_icon_app_release();
    lv_memzero(&s_ui, sizeof(s_ui));
}

lv_obj_t *ui_page_get_screen(void)
{
    return s_ui.screen;
}

uint32_t ui_page_count(void)
{
    return page_total();
}

uint32_t ui_page_current(void)
{
    return s_ui.page;
}

void ui_page_show(uint32_t page)
{
    ui_page_menu_close();       /* the menu belongs to the page being replaced */
    if (s_ui.settle != NULL) {
        lv_timer_del(s_ui.settle);
        s_ui.settle = NULL;
    }
    ui_page_bind(page);
}

bool ui_page_register(const char *title, const char *app_id, const char *script,
                      uint32_t frame_color, uint32_t pill_color,
                      const char *icon_sel, const char *icon_unsel)
{
    /* Must be called before ui_page_screen_init() — the dot widgets are created
     * once at init time with the live page count, so registering afterwards
     * would leave the new page without a dot. */
    if (s_ui.screen != NULL) {
        return false;
    }
    if (s_page_count >= UI_PAGE_MAX) {
        return false;
    }

    ui_page_load_defaults();

    s_pages[s_page_count++] = (ui_app_desc_t){
        .title       = title,
        .app_id      = app_id,
        .script      = script,
        .frame_color = frame_color,
        .pill_color  = pill_color,
        .icon_sel    = icon_sel,
        .icon_unsel  = icon_unsel,
    };
    return true;
}

bool ui_page_add_app(const char *title, const char *app_id, const char *location)
{
    uint8_t index;

    /* Like ui_page_register(): the dot row is sized when the screen is built. */
    if (s_ui.screen != NULL || title == NULL || app_id == NULL) {
        return false;
    }
    ui_page_load_defaults();
    if (s_page_count >= UI_PAGE_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < s_page_count; i++) {
        if (s_pages[i].app_id != NULL && strcmp(s_pages[i].app_id, app_id) == 0) {
            return false;
        }
    }

    if (!ui_page_text_ready()) {
        return false;
    }
    index = s_page_count;
    if (strlcpy(s_app_text[index].title, title, UI_PAGE_APP_TITLE_LEN) >= UI_PAGE_APP_TITLE_LEN ||
            strlcpy(s_app_text[index].app_id, app_id, UI_PAGE_APP_ID_LEN) >= UI_PAGE_APP_ID_LEN ||
            strlcpy(s_app_text[index].location, location != NULL ? location : "",
                    UI_PAGE_APP_LOCATION_LEN) >= UI_PAGE_APP_LOCATION_LEN) {
        return false;
    }
    s_pages[index] = (ui_app_desc_t){
        .title       = s_app_text[index].title,
        .app_id      = s_app_text[index].app_id,
        .script      = NULL,    /* the owner launches installed apps by app_id */
        .frame_color = UI_PAGE_APP_ACCENT,
        .pill_color  = UI_PAGE_APP_ACCENT,
        .icon_sel    = UI_ICON_APP_NAME,
        .icon_unsel  = UI_ICON_APP_NAME,
        .installed   = true,
        .location    = location != NULL ? s_app_text[index].location : NULL,
    };
    s_page_count++;
    return true;
}

void ui_page_clear_apps(void)
{
    ui_page_set_provider(0, NULL, NULL);
    if (s_ui.screen == NULL && s_page_count > s_static_page_count) {
        s_page_count = s_static_page_count;
    }
}

void ui_page_show_toast(const char *text)
{
    ui_page_toast(text);
}

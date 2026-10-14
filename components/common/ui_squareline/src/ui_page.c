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
#include <string.h>

#include "ui.h"
#include "ui_icon_2048.h"
#include "ui_img_sd.h"
#include "ui_squareline_priv.h"

/* Duration of the slide animations defined in ui.c. The page is re-bound once
 * the slide has finished, which is what the old per-screen code achieved by
 * passing the same value as _ui_screen_change()'s delay. */
#define UI_PAGE_ANIM_MS 1000

/* Indicator dots: one per page at y=185, 22 px apart, centred on the screen. The
 * selected one is drawn larger and in the accent colour.
 *
 * The dot index *is* the page index, so the highlight always walks the row in
 * step with PREV/NEXT and a new page cannot get it out of sync. The generated
 * screens instead hard-coded a dot per screen with a rotation offset of 3
 * (Screen1 lit the fourth dot), which is why adding 2048 as an eighth page made
 * the highlight jump: it claimed the eighth dot while the seven older pages
 * still covered dots 0..6. */
#define UI_PAGE_DOT_SPACING     22
#define UI_PAGE_DOT_Y           185
#define UI_PAGE_DOT_SIZE        15
#define UI_PAGE_DOT_SIZE_SEL    18
#define UI_PAGE_DOT_COLOR       0x384038
#define UI_PAGE_DOT_COLOR_SEL   0xF0C430

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
} ui_app_desc_t;

/* Maximum number of pages that can be registered at runtime.  The static table
 * below counts as the first batch; ui_page_register() fills the rest. */
#define UI_PAGE_MAX 16

/* Pages registered so far.  s_page_count is the live count; it grows as
 * ui_page_register() is called.  The first s_static_page_count entries are the
 * compile-time defaults and are never modified at runtime. */
static ui_app_desc_t s_pages[UI_PAGE_MAX];
static uint8_t       s_page_count       = 0;
static uint8_t       s_static_page_count = 0;

/* Ring accessor: index modulo live count. */
static inline uint8_t page_ring(int idx)
{
    int n = (int)s_page_count;
    return (uint8_t)(((idx % n) + n) % n);
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
    lv_obj_t **dots;            /* allocated in screen_init, length = s_page_count */
    lv_timer_t *settle;         /* fires when a slide finishes, then re-binds */
    uint8_t   page;
    uint8_t   pending;          /* page to bind when s_ui.settle fires */
} s_ui;

/* ------------------------------------------------------------------ helpers */

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
static void ui_page_bind(uint8_t page)
{
    lv_obj_t *slots[5];

    if (s_ui.screen == NULL || page >= s_page_count) {
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
    const ui_app_desc_t *cur  = &s_pages[page];
    const ui_app_desc_t *next = &s_pages[page_ring((int)page + 1)];
    const ui_app_desc_t *prev = &s_pages[page_ring((int)page - 1)];
    const ui_app_desc_t *next2 = &s_pages[page_ring((int)page + 2)];
    const ui_app_desc_t *prev2 = &s_pages[page_ring((int)page - 2)];

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
    lv_obj_set_style_border_color(s_ui.frame, lv_color_hex(cur->frame_color),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_page_block_fill(s_ui.pill, cur->pill_color);

    for (uint8_t i = 0; i < s_page_count; i++) {
        bool sel = (i == page);
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
static void ui_page_slide(uint8_t target, bool forward)
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

/* The three user actions the screen offers. Both the on-screen hit targets and
 * the board's physical keys funnel through these, so a key press behaves exactly
 * like a tap: same slide animation, same settle timer, same select dispatch.
 *
 * The left-hand "PREV" caption advanced to the next screen in the generated
 * chain and the right-hand "NEXT" one went back; those bindings are kept as they
 * were, so the ring index moves opposite to the captions. */
void ui_page_prev(void)
{
    ui_page_slide(page_ring((int)s_ui.page - 1), false);
}

void ui_page_next(void)
{
    ui_page_slide(page_ring((int)s_ui.page + 1), true);
}

/* "M OPEN": report the selection and let the owner decide what to open. */
void ui_page_select(void)
{
    const ui_app_desc_t *p;

    if (s_ui.screen == NULL) {
        return;
    }
    p = &s_pages[s_ui.page];
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

    /* Populate the runtime page table from the compile-time defaults on first
     * call.  ui_page_register() may have been called before screen_init to
     * append extra pages; those sit after the static ones and are left alone. */
    if (s_page_count == 0) {
        s_static_page_count = (uint8_t)(sizeof(s_default_pages) / sizeof(s_default_pages[0]));
        for (uint8_t i = 0; i < s_static_page_count && i < UI_PAGE_MAX; i++) {
            s_pages[i] = s_default_pages[i];
        }
        s_page_count = s_static_page_count;
    }

    /* Allocate the dot-widget pointer array now that we know the live count. */
    s_ui.dots = lv_malloc(s_page_count * sizeof(lv_obj_t *));
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
    root = ui_page_block(s_ui.screen, 804, 490, 0, 4, 0);
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
    bar = ui_page_block(root, 791, 30, -1, -222, 0);
    ui_page_block_fill(bar, 0x080C08);
    ui_page_block_fill(ui_page_block(bar, 9, 39, -388, -1, 0), 0xF8E458);

    ui_page_label(root, 88, 54, 313, -221, "9:51", 0xFAFBF5, false);

    /* Battery gauge. */
    obj = lv_bar_create(root);
    lv_bar_set_value(obj, 70, LV_ANIM_OFF);
    lv_bar_set_start_value(obj, 0, LV_ANIM_OFF);
    lv_obj_set_width(obj, 32);
    lv_obj_set_height(obj, 9);
    lv_obj_set_x(obj, 367);
    lv_obj_set_y(obj, -225);
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
    bar = ui_page_block(root, 240, 41, 0, -176, 0);
    ui_page_block_fill(bar, 0x080C08);
    obj = ui_page_block(bar, 17, 16, -88, 2, 75);
    ui_page_block_fill(obj, 0xF8AC10);

    ui_page_label(root, 93, 54, 1, -169, "会议纪要", 0xFAFBF5, true);

    /* Footer strip: top border only. */
    bar = ui_page_block(root, 801, 43, 2, 216, 0);
    ui_page_block_fill(bar, 0x080C08);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x384038), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_pad(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    footer_prev = ui_page_label(root, 93, 45, -321, 221, "PREV", 0xF0C428, true);
    footer_open = ui_page_label(root, 93, 49, -1, 221, "M OPEN", 0xF0C428, true);

    /* Page caption — rebound on every page change; empty string until ui_page_bind(0) below. */
    s_ui.title = ui_page_label(root, 360, 45, 4, -18, "", 0x788078, true);
    lv_textarea_set_one_line(s_ui.title, true);
    lv_obj_set_style_text_align(s_ui.title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_ui.title, lv_color_hex(0x786028), LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_page_label(root, 135, 58, -316, -213, "LANGUAGE", 0xFAFBF5, false);

    /* Frame + pill highlighting the selected icon. Colours rebound by ui_page_bind(0). */
    s_ui.frame = ui_page_block(root, 191, 174, 0, 83, 20);
    lv_obj_set_style_border_color(s_ui.frame, lv_color_hex(0x000000),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_ui.frame, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_ui.frame, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_ui.pill = ui_page_block(s_ui.frame, 152, 13, 0, -66, 10);
    ui_page_block_fill(s_ui.pill, 0x000000);

    /* Visible carousel slots — sources will be overwritten by ui_page_bind(0)
     * below; passing NULL here is safe because set_src accepts NULL. */
    s_ui.icon_mid   = ui_page_icon(root, NULL,  0,    89, 128, false, true);
    s_ui.icon_right = ui_page_icon(root, NULL,  217,  40,  98, false, false);
    s_ui.icon_left  = ui_page_icon(root, NULL, -217,  40,  98, false, false);

    /* Indicator dots, left to right. */
    for (uint8_t i = 0; i < s_page_count; i++) {
        int x = ((int)i * 2 - (s_page_count - 1)) * UI_PAGE_DOT_SPACING / 2;
        s_ui.dots[i] = ui_page_block(root, UI_PAGE_DOT_SIZE, UI_PAGE_DOT_SIZE,
                                     x, UI_PAGE_DOT_Y, 75);
        ui_page_block_fill(s_ui.dots[i], UI_PAGE_DOT_COLOR);
    }

    /* Static arrow glyphs beside the footer buttons. */
    ui_page_icon(root, "ui_img_1793379158",  367, 217, 64, false, false);
    ui_page_icon(root, "ui_img_1111249434", -380, 218, 64, false, false);

    /* Hidden slots the incoming neighbour flies in from.
     * Sources are overwritten by ui_page_bind(0) below. */
    s_ui.icon_right_in = ui_page_icon(root, NULL, -3, 92, 98, true, false);
    lv_obj_add_state(s_ui.icon_right_in, LV_STATE_USER_1);
    s_ui.icon_left_in  = ui_page_icon(root, NULL,  0, 89, 98, true, false);

    footer_next = ui_page_label(root, 93, 66, 325, 230, "NEXT", 0xF0C428, true);
    lv_obj_set_flex_flow(footer_next, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer_next, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    btn = ui_page_button(root, 101, 30, 338, 220, true);
    lv_obj_add_event_cb(btn, ui_page_right_cb, LV_EVENT_ALL, NULL);

    btn = ui_page_button(root, 97, 22, -344, 219, false);
    lv_obj_add_event_cb(btn, ui_page_left_cb, LV_EVENT_ALL, NULL);

    btn = ui_page_button(root, 80, 22, -4, 220, true);
    lv_obj_add_event_cb(btn, ui_page_open_cb, LV_EVENT_ALL, NULL);

    lv_obj_clear_flag(footer_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(footer_open, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(footer_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(footer_prev);
    lv_obj_move_foreground(footer_open);
    lv_obj_move_foreground(footer_next);

    lv_obj_add_flag(s_ui.icon_mid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_ui.icon_mid, ui_page_mid_icon_cb, LV_EVENT_DOUBLE_CLICKED, NULL);

    /* Register touch events for swipe navigation */
    lv_obj_add_event_cb(s_ui.screen, ui_page_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.screen, ui_page_touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, ui_page_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, ui_page_touch_event_cb, LV_EVENT_RELEASED, NULL);

    ui_page_bind(0);
}

void ui_page_screen_destroy(void)
{
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
    lv_memzero(&s_ui, sizeof(s_ui));
}

lv_obj_t *ui_page_get_screen(void)
{
    return s_ui.screen;
}

uint8_t ui_page_count(void)
{
    return s_page_count;
}

uint8_t ui_page_current(void)
{
    return s_ui.page;
}

void ui_page_show(uint8_t page)
{
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

    /* Populate from the compile-time defaults on first register call if they
     * haven't been copied yet (screen_init does this too, but register may run
     * first). */
    if (s_page_count == 0) {
        s_static_page_count = (uint8_t)(sizeof(s_default_pages) / sizeof(s_default_pages[0]));
        for (uint8_t i = 0; i < s_static_page_count && i < UI_PAGE_MAX; i++) {
            s_pages[i] = s_default_pages[i];
        }
        s_page_count = s_static_page_count;
    }

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

#include "clip_stock_board.h"
#include "clip_feed_widgets.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#define TEXT 0xF5F5F7
#define MUTED 0x8E8E93
#define PANEL 0x1C1C1E
#define UP 0xFF453A
#define DOWN 0x30D158
#define MAX_STOCKS 20
#define POINTS 64
static const struct {
    const char *symbol, *name, *company;
} presets[] = {{"us.DJI", "Dow Jones", "Dow Jones Industrial Average"},
               {"usAAPL", "AAPL", "Apple Inc."},
               {"usBA", "BA", "The Boeing Company"},
               {"usBRK.B", "BRK-B", "伯克希尔哈撒韦"},
               {"usDIS", "DIS", "The Walt Disney Company"},
               {"usGE", "GE", "GE Aerospace"},
               {"usHD", "HD", "The Home Depot, Inc."},
               {"usNKE", "NKE", "NIKE, Inc."},
               {"usSBUX", "SBUX", "Starbucks Corporation"},
               {"hk00941", "中国移动", "CHINA MOBILE"},
               {"sh600000", "浦发银行", "上海 · CNY"},
               {"sh000001", "上证指数", "上海 · CNY"},
               {"sz399001", "深证成指", "深圳 · CNY"},
               {"sz399006", "创业板指", "深圳 · CNY"},
               {"usMSFT", "MSFT", "Microsoft Corporation"},
               {"usNVDA", "NVDA", "NVIDIA Corporation"},
               {"usTSLA", "TSLA", "Tesla, Inc."},
               {"usGOOGL", "GOOGL", "Alphabet Inc."},
               {"hk00700", "腾讯控股", "TENCENT"},
               {"sh600519", "贵州茅台", "上海 · CNY"}};
typedef struct {
    struct clip_stock_board_t *board;
    int index;
} stock_tap_t;
struct clip_stock_board_t {
    clip_fonts_t fonts;
    lv_obj_t *root, *list, *date, *status, *sheet, *search, *input, *results, *keyboard, *search_hint;
    lv_obj_t *title, *company, *price, *change, *period, *chart, *chart_note, *plot, *lower, *upper,
        *axis_start, *axis_end, *range_tabs[5], *metrics[12];
    lv_obj_t *rows[MAX_STOCKS], *names[MAX_STOCKS], *companies[MAX_STOCKS], *prices[MAX_STOCKS],
        *badges[MAX_STOCKS], *minis[MAX_STOCKS];
    lv_point_precise_t mini_points[MAX_STOCKS][POINTS], points[POINTS];
    stock_tap_t taps[MAX_STOCKS], range_taps[5], search_taps[21];
    cJSON *payload;
    char *raw;
    char status_text[160], query[20], pending[20];
    int count, selected, range, point_count;
    bool refresh, query_ready;
    uint32_t action_id, handled_id;
    char *action_request;
    cJSON *search_stock;
};
static void render(clip_stock_board_handle_t b);
static const cJSON *card_at(clip_stock_board_handle_t b, int i) {
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(b->payload, "cards"), i);
}
static const char *display_name(const cJSON *c) {
    const char *s = cf_str(c, "symbol");
    for (unsigned i = 0; i < sizeof(presets) / sizeof(*presets); i++) {
        if (!strcmp(s, presets[i].symbol)) {
            return presets[i].name;
        }
    }
    return *cf_str(c, "name") ? cf_str(c, "name") : s;
}
static const char *company(const cJSON *c) {
    const char *s = cf_str(c, "symbol");
    for (unsigned i = 0; i < sizeof(presets) / sizeof(*presets); i++) {
        if (!strcmp(s, presets[i].symbol)) {
            return presets[i].company;
        }
    }
    return *cf_str(c, "company") ? cf_str(c, "company") : cf_str(c, "name");
}
static uint32_t tone(const cJSON *c) {
    return cf_num(c, "change") < 0 ? DOWN : UP;
}
static void price_text(lv_obj_t *o, const cJSON *c) {
    double p = cf_num(c, "price");
    const char *currency = cf_str(c, "currency");
    if (isfinite(p)) {
        lv_label_set_text_fmt(o, "%s%.2f",
                              !strcmp(currency, "USD")   ? "US$"
                              : !strcmp(currency, "HKD") ? "HK$"
                                                         : "¥",
                              p);
    } else {
        lv_label_set_text(o, "--");
    }
}
static int plot(clip_stock_board_handle_t b, const cJSON *c, lv_obj_t *line, lv_point_precise_t *points,
                int w, int h, bool full) {
    const cJSON *series = cJSON_GetObjectItemCaseSensitive(c, "series"), *range = NULL;
    if (full && b->range) {
        const char *keys[] = {"week", "month", "quarter", "year"};
        range = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c, "ranges"),
                                                 keys[b->range - 1]);
        series = cJSON_GetObjectItemCaseSensitive(range, "series");
    }
    int n = cJSON_GetArraySize(series);
    if (n < 2 || n > POINTS) {
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        return 0;
    }
    double lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(series, i);
        if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble <= 0) {
            lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
            return 0;
        }
        lo = fmin(lo, v->valuedouble);
        hi = fmax(hi, v->valuedouble);
    }
    double pad = fmax((hi - lo) * .06, hi * .0001);
    for (int i = 0; i < n; i++) {
        points[i].x = (int)lround((double)i * (w - 1) / (n - 1));
        points[i].y = (int)lround((hi + pad - cJSON_GetArrayItem(series, i)->valuedouble) /
                                  (hi - lo + 2 * pad) * (h - 1));
    }
    lv_line_set_points(line, points, (uint32_t)n);
    lv_obj_set_style_line_color(line, lv_color_hex(tone(c)), 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
    if (full) {
        lv_label_set_text_fmt(b->upper, "%.2f", hi);
        lv_label_set_text_fmt(b->lower, "%.2f", lo);
        lv_label_set_text(b->axis_start, b->range ? cf_str(range, "start") : "09:30");
        lv_label_set_text(b->axis_end, b->range                                ? cf_str(range, "end")
                                       : !strcmp(cf_str(c, "currency"), "CNY") ? "15:00"
                                                                               : "16:00");
        double first = cJSON_GetArrayItem(series, 0)->valuedouble,
               last = cJSON_GetArrayItem(series, n - 1)->valuedouble;
        if (b->range) {
            lv_label_set_text_fmt(b->change, "%+.2f  %+.2f%%", last - first, (last / first - 1) * 100);
        }
    }
    return n;
}
static void chart_fill(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    if (b->point_count < 2) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(b->chart, &area);
    lv_draw_triangle_dsc_t *d = lv_malloc(sizeof(*d));
    if (!d) {
        return;
    }
    lv_draw_triangle_dsc_init(d);
    d->color = lv_color_hex(tone(card_at(b, b->selected)));
    d->opa = 36;
    for (int i = 1; i < b->point_count; i++) {
        lv_point_precise_t a = {area.x1 + b->points[i - 1].x, area.y1 + b->points[i - 1].y},
                           p = {area.x1 + b->points[i].x, area.y1 + b->points[i].y};
        d->p[0] = a;
        d->p[1] = p;
        d->p[2] = (lv_point_precise_t){p.x, area.y2};
        lv_draw_triangle(layer, d);
        d->p[0] = a;
        d->p[1] = (lv_point_precise_t){p.x, area.y2};
        d->p[2] = (lv_point_precise_t){a.x, area.y2};
        lv_draw_triangle(layer, d);
    }
    lv_free(d);
}
static void show_detail(clip_stock_board_handle_t b, int index) {
    b->selected = index;
    b->range = 0;
    lv_obj_remove_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
    render(b);
}
static void row_tap(lv_event_t *e) {
    stock_tap_t *t = lv_event_get_user_data(e);
    show_detail(t->board, t->index);
}
static void close_sheet(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_add_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
}
static void refresh(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    b->refresh = true;
}
static void range_tap(lv_event_t *e) {
    stock_tap_t *t = lv_event_get_user_data(e);
    t->board->range = t->index;
    render(t->board);
}
static void close_search(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_add_flag(b->search, LV_OBJ_FLAG_HIDDEN);
}
static void search_pick(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    const char *symbol = cf_str(b->search_stock, "symbol");
    if (!*symbol) {
        return;
    }
    snprintf(b->pending, sizeof(b->pending), "%s", symbol);
    cJSON *s = cJSON_CreateString(symbol);
    cf_action(&b->action_request, &b->action_id, "stock_add", NULL, "symbol", s);
    cJSON_Delete(s);
    lv_label_set_text(b->search_hint, "正在保存自选…");
}
static void search_changed(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    const char *query = lv_textarea_get_text(b->input);
    lv_obj_clean(b->results);
    cJSON_Delete(b->search_stock);
    b->search_stock = NULL;
    b->handled_id = b->action_id; /* Ignore an in-flight result after the input changes. */
    free(b->action_request);
    b->action_request = NULL;
    if (strlen(query) != 6) {
        lv_label_set_text(b->search_hint, "直接输入6位数字，例如 600000、000001");
        return;
    }
    for (int i = 0; i < 6; i++) {
        if (query[i] < '0' || query[i] > '9') {
            lv_label_set_text(b->search_hint, "请输入6位数字，无需市场或拼音前缀");
            return;
        }
    }
    cf_action(&b->action_request, &b->action_id, "stock_search", query, NULL, NULL);
    lv_label_set_text(b->search_hint, "正在通过背夹 Wi-Fi 查询…");
}
static void open_search(lv_event_t *e) {
    clip_stock_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_remove_flag(b->search, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(b->input, "");
    lv_label_set_text(b->search_hint, "直接输入6位数字 · 背夹 Wi-Fi 在线查询");
}
static void render(clip_stock_board_handle_t b) {
    const cJSON *fetched = cJSON_GetObjectItemCaseSensitive(b->payload, "fetchedAt");
    if (cJSON_IsNumber(fetched)) {
        uint64_t sec = (uint64_t)(fetched->valuedouble / 1000) + 28800;
        time_t t = (time_t)sec;
        struct tm tm;
        gmtime_r(&t, &tm);
        lv_label_set_text_fmt(b->date, "%d月%d日", tm.tm_mon + 1, tm.tm_mday);
    }
    for (int i = 0; i < MAX_STOCKS; i++) {
        const cJSON *c = card_at(b, i);
        if (!c) {
            lv_obj_add_flag(b->rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(b->rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(b->names[i], display_name(c));
        lv_label_set_text(b->companies[i], company(c));
        price_text(b->prices[i], c);
        double change = cf_num(c, "change");
        if (isfinite(change)) {
            lv_label_set_text_fmt(b->badges[i], "%+.2f", change);
        } else {
            lv_label_set_text(b->badges[i], "--");
        }
        lv_obj_set_style_bg_color(b->badges[i], lv_color_hex(tone(c)), 0);
        plot(b, c, b->minis[i], b->mini_points[i], 78, 26, false);
    }
    lv_label_set_text(b->status, b->status_text);
    const cJSON *c = card_at(b, b->selected);
    lv_label_set_text(b->title, display_name(c));
    lv_label_set_text(b->company, company(c));
    price_text(b->price, c);
    double change = cf_num(c, "change"), percent = cf_num(c, "percent");
    if (isfinite(change) && isfinite(percent)) {
        lv_label_set_text_fmt(b->change, "%+.2f  %+.2f%%", change, percent);
    } else {
        lv_label_set_text(b->change, "--");
    }
    lv_obj_set_style_text_color(b->change, lv_color_hex(tone(c)), 0);
    lv_label_set_text_fmt(b->period, "%s · %s · %s", b->range ? "历史走势" : "今日分时", cf_str(c, "market"),
                          cf_str(c, "currency"));
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_bg_opa(b->range_tabs[i], i == b->range ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    b->point_count = plot(b, c, b->plot, b->points, 364, 270, true);
    lv_obj_invalidate(b->chart);
    if (b->point_count) {
        lv_obj_add_flag(b->chart_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(b->chart_note, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(b->chart_note, "该时段暂无可用走势");
        lv_label_set_text(b->upper, "");
        lv_label_set_text(b->lower, "");
        lv_label_set_text(b->axis_start, "");
        lv_label_set_text(b->axis_end, "");
    }
    const char *keys[] = {"open",          "yearHigh", "high",      "yearLow", "low",    "volume",
                          "previousClose", "pe",       "marketCap", "percent", "change", "price"};
    for (int i = 0; i < 12; i++) {
        double v = cf_num(c, keys[i]);
        if (!isfinite(v)) {
            lv_label_set_text(b->metrics[i], "--");
        } else if (v >= 1e8) {
            lv_label_set_text_fmt(b->metrics[i], "%.2f亿", v / 1e8);
        } else if (v >= 1e4) {
            lv_label_set_text_fmt(b->metrics[i], "%.1f万", v / 1e4);
        } else {
            lv_label_set_text_fmt(b->metrics[i], "%.2f", v);
        }
    }
}
clip_stock_board_handle_t clip_stock_board_create(lv_obj_t *screen) {
    clip_stock_board_handle_t b = calloc(1, sizeof(*b));
    if (!b) {
        return NULL;
    }
    cf_load(&b->fonts);
    b->root = cf_box(screen, 0, 0, 480, 800, 0, 0);
    cf_label(&b->fonts, b->root, 18, 20, 265, "股市", 3, TEXT);
    b->date = cf_label(&b->fonts, b->root, 18, 58, 280, "自选行情", 3, MUTED);
    cf_button(&b->fonts, b->root, 310, 26, 62, 46, "刷新", PANEL, refresh, b);
    cf_button(&b->fonts, b->root, 382, 26, 80, 46, "搜索", PANEL, open_search, b);
    b->list = cf_box(b->root, 18, 112, 444, 600, 0, 0);
    cf_scroll(b->list, LV_DIR_VER);
    for (int i = 0; i < MAX_STOCKS; i++) {
        b->rows[i] = cf_box(b->list, 0, i * 83, 444, 83, 0, 0);
        b->taps[i] = (stock_tap_t){b, i};
        lv_obj_add_flag(b->rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b->rows[i], row_tap, LV_EVENT_CLICKED, &b->taps[i]);
        b->names[i] = cf_label(&b->fonts, b->rows[i], 0, 10, 228, "--", 2, TEXT);
        b->companies[i] = cf_label(&b->fonts, b->rows[i], 0, 44, 228, "", 0, MUTED);
        b->prices[i] = cf_label(&b->fonts, b->rows[i], 286, 9, 158, "--", 1, TEXT);
        lv_obj_set_style_text_align(b->prices[i], LV_TEXT_ALIGN_RIGHT, 0);
        b->badges[i] = cf_label(&b->fonts, b->rows[i], 363, 39, 81, "--", 0, TEXT);
        lv_obj_set_style_bg_opa(b->badges[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b->badges[i], 4, 0);
        lv_obj_set_style_pad_ver(b->badges[i], 3, 0);
        lv_obj_set_style_text_align(b->badges[i], LV_TEXT_ALIGN_CENTER, 0);
        b->minis[i] = lv_line_create(b->rows[i]);
        lv_obj_set_pos(b->minis[i], 246, 36);
        lv_obj_set_style_line_width(b->minis[i], 2, 0);
        cf_box(b->rows[i], 0, 82, 444, 1, 0x2C2C2E, 0);
    }
    b->status = cf_label(&b->fonts, b->root, 18, 730, 444, "等待行情更新", 0, MUTED);
    cf_label(&b->fonts, b->root, 18, 764, 444, "点选查看详情 · 上下滑动 · SET 返回主页", 0, MUTED);
    b->sheet = cf_box(b->root, 0, 72, 480, 728, PANEL, 28);
    cf_box(b->sheet, 218, 8, 44, 5, 0x636366, 3);
    cf_button(&b->fonts, b->sheet, 20, 23, 48, 42, "×", 0x2C2C2E, close_sheet, b);
    cf_button(&b->fonts, b->sheet, 382, 23, 78, 42, "刷新", 0x2C2C2E, refresh, b);
    b->title = cf_label(&b->fonts, b->sheet, 18, 77, 444, "--", 4, TEXT);
    b->company = cf_label(&b->fonts, b->sheet, 18, 128, 444, "", 0, MUTED);
    cf_box(b->sheet, 18, 157, 444, 1, 0x38383A, 0);
    b->price = cf_label(&b->fonts, b->sheet, 18, 166, 245, "--", 2, TEXT);
    b->change = cf_label(&b->fonts, b->sheet, 255, 168, 207, "--", 1, UP);
    b->period = cf_label(&b->fonts, b->sheet, 18, 196, 444, "", 0, MUTED);
    const char *ranges[] = {"1天", "1周", "1个月", "3个月", "1年"};
    for (int i = 0; i < 5; i++) {
        b->range_taps[i] = (stock_tap_t){b, i};
        b->range_tabs[i] = cf_button(&b->fonts, b->sheet, 18 + i * 89, 227, 81, 35, ranges[i], 0x3A3A3C,
                                     range_tap, &b->range_taps[i]);
    }
    b->chart = cf_box(b->sheet, 18, 277, 364, 270, PANEL, 0);
    lv_obj_add_event_cb(b->chart, chart_fill, LV_EVENT_DRAW_MAIN, b);
    for (int i = 0; i < 4; i++) {
        cf_box(b->chart, 0, i * 89, 364, 1, 0x2D2D2F, 0);
    }
    b->plot = lv_line_create(b->chart);
    lv_obj_set_style_line_width(b->plot, 2, 0);
    b->upper = cf_label(&b->fonts, b->sheet, 389, 277, 76, "", 0, TEXT);
    b->lower = cf_label(&b->fonts, b->sheet, 389, 522, 76, "", 0, TEXT);
    b->chart_note = cf_label(&b->fonts, b->chart, 45, 116, 300, "等待真实行情", 1, MUTED);
    b->axis_start = cf_label(&b->fonts, b->sheet, 18, 552, 210, "", 0, MUTED);
    b->axis_end = cf_label(&b->fonts, b->sheet, 260, 552, 200, "", 0, MUTED);
    lv_obj_set_style_text_align(b->axis_end, LV_TEXT_ALIGN_RIGHT, 0);
    const char *titles[] = {"今日开盘价", "52周最高价", "今日最高价", "52周最低价", "今日最低价", "成交量",
                            "昨日收盘价", "市盈率",     "市值(亿)",   "涨跌幅(%)",  "涨跌额",     "最新价"};
    for (int i = 0; i < 12; i++) {
        int x = 18 + (i % 2) * 226, y = 583 + (i / 2) * 19;
        cf_label(&b->fonts, b->sheet, x, y, 115, titles[i], 0, MUTED);
        b->metrics[i] = cf_label(&b->fonts, b->sheet, x + 114, y, 104, "--", 0, TEXT);
        lv_obj_set_style_text_align(b->metrics[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    cf_label(&b->fonts, b->sheet, 18, 706, 444, "腾讯财经 · 行情可能延迟", 0, MUTED);
    lv_obj_add_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
    b->search = cf_box(b->root, 0, 0, 480, 800, 0x101012, 0);
    cf_label(&b->fonts, b->search, 18, 22, 300, "搜索股票", 3, TEXT);
    cf_button(&b->fonts, b->search, 382, 20, 80, 40, "取消", PANEL, close_search, b);
    b->input = lv_textarea_create(b->search);
    lv_obj_set_pos(b->input, 18, 79);
    lv_obj_set_size(b->input, 444, 52);
    lv_textarea_set_one_line(b->input, true);
    lv_textarea_set_max_length(b->input, 6);
    lv_textarea_set_accepted_chars(b->input, "0123456789");
    lv_textarea_set_placeholder_text(b->input, "输入6位股票代码");
    lv_obj_set_style_text_font(b->input, b->fonts.fonts[1], 0);
    lv_obj_set_style_text_color(b->input, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_color(b->input, lv_color_hex(PANEL), 0);
    lv_obj_add_event_cb(b->input, search_changed, LV_EVENT_VALUE_CHANGED, b);
    b->search_hint = cf_label(&b->fonts, b->search, 18, 143, 444, "搜索名称或代码", 0, MUTED);
    b->results = cf_box(b->search, 18, 180, 444, 310, 0x101012, 0);
    cf_scroll(b->results, LV_DIR_VER);
    b->keyboard = lv_keyboard_create(b->search);
    lv_obj_set_size(b->keyboard, 480, 280);
    lv_obj_align(b->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(b->keyboard, b->input);
    lv_keyboard_set_mode(b->keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_add_flag(b->search, LV_OBJ_FLAG_HIDDEN);
    snprintf(b->status_text, sizeof(b->status_text), "等待联网更新");
    render(b);
    return b;
}
void clip_stock_board_delete(clip_stock_board_handle_t b) {
    if (!b) {
        return;
    }
    lv_obj_delete(b->root);
    cJSON_Delete(b->payload);
    cJSON_Delete(b->search_stock);
    free(b->action_request);
    free(b->raw);
    cf_free(&b->fonts);
    free(b);
}
void clip_stock_board_update(clip_stock_board_handle_t b, const char *raw, int selected, const char *status) {
    (void)selected;
    if (!b) {
        return;
    }
    bool changed = false;
    if (raw && *raw && (!b->raw || strcmp(b->raw, raw))) {
        cJSON *p = cJSON_Parse(raw);
        int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "cards"));
        if (n > 0 && n <= MAX_STOCKS) {
            char *copy = strdup(raw);
            if (copy) {
                cJSON_Delete(b->payload);
                free(b->raw);
                b->payload = p;
                p = NULL;
                b->raw = copy;
                b->count = n;
                b->selected = b->selected < n ? b->selected : 0;
                changed = true;
            }
        }
        cJSON_Delete(p);
        if (*b->pending) {
            for (int i = 0; i < b->count; i++) {
                if (!strcmp(b->pending, cf_str(card_at(b, i), "symbol"))) {
                    lv_obj_add_flag(b->search, LV_OBJ_FLAG_HIDDEN);
                    b->selected = i;
                    lv_obj_remove_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
                    b->pending[0] = 0;
                    break;
                }
            }
        }
    }
    if (status && strcmp(status, b->status_text)) {
        snprintf(b->status_text, sizeof(b->status_text), "%s", status);
        changed = true;
    }
    if (changed) {
        render(b);
    }
}
int clip_stock_board_selected(clip_stock_board_handle_t b) {
    return b ? b->selected : 0;
}
bool clip_stock_board_refresh_requested(clip_stock_board_handle_t b) {
    bool r = b && b->refresh;
    if (b) {
        b->refresh = false;
    }
    return r;
}
void clip_stock_board_key(clip_stock_board_handle_t b, const char *key) {
    if (!b || !key) {
        return;
    }
    if (!strcmp(key, "confirm")) {
        if (b->count && lv_obj_has_flag(b->sheet, LV_OBJ_FLAG_HIDDEN)) {
            show_detail(b, b->selected);
        } else {
            b->refresh = true;
        }
    } else if (b->count) {
        b->selected = (b->selected + (!strcmp(key, "left") ? b->count - 1 : 1)) % b->count;
        lv_obj_scroll_to_y(b->list, b->selected * 83, LV_ANIM_OFF);
        render(b);
    }
}
const char *clip_stock_board_query(clip_stock_board_handle_t b) {
    if (!b || !b->query_ready) {
        return NULL;
    }
    b->query_ready = false;
    return b->query;
}
void clip_stock_board_query_result(clip_stock_board_handle_t b, bool ok) {
    if (b) {
        lv_label_set_text(b->search_hint,
                          ok ? "已加入自选，等待行情更新…" : "无法添加，请检查代码或自选数量（最多20只）");
    }
}

char *clip_stock_board_take_action(clip_stock_board_handle_t b) {
    if (!b) {
        return NULL;
    }
    char *r = b->action_request;
    b->action_request = NULL;
    return r;
}
void clip_stock_board_action_result(clip_stock_board_handle_t b, const char *raw) {
    if (!b || !raw) {
        return;
    }
    cJSON *j = cJSON_Parse(raw);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "requestId");
    if (!cJSON_IsNumber(id) || (uint32_t)id->valueint != b->action_id || b->handled_id == b->action_id ||
        strcmp(cf_str(j, "state"), "complete")) {
        cJSON_Delete(j);
        return;
    }
    b->handled_id = b->action_id;
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok"))) {
        lv_label_set_text(b->search_hint, cf_str(j, "error"));
        cJSON_Delete(j);
        return;
    }
    if (!strcmp(cf_str(j, "op"), "stock_search")) {
        cJSON_Delete(b->search_stock);
        b->search_stock = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "stock"), true);
        lv_obj_clean(b->results);
        lv_obj_t *row = cf_box(b->results, 0, 0, 440, 120, 0x222225, 14);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, search_pick, LV_EVENT_CLICKED, b);
        cf_label(&b->fonts, row, 14, 12, 280, cf_str(b->search_stock, "name"), 3, TEXT);
        cf_label(&b->fonts, row, 14, 58, 270, cf_str(b->search_stock, "symbol") + 2, 1, MUTED);
        lv_obj_t *p = cf_label(&b->fonts, row, 290, 18, 136, "", 1, TEXT);
        price_text(p, b->search_stock);
        cf_label(&b->fonts, row, 14, 87, 410, "Wi-Fi 实时查询 · 点选查看并加入自选", 0, MUTED);
        lv_label_set_text(b->search_hint, "已通过背夹 Wi-Fi 获取报价");
    } else if (!strcmp(cf_str(j, "op"), "stock_add")) {
        lv_label_set_text(b->search_hint, "已保存，正在打开股票详情…");
    }
    cJSON_Delete(j);
}

cJSON *clip_stock_board_inspect(clip_stock_board_handle_t b) {
    if (!b) {
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "view", "stocks");
    cJSON_AddBoolToObject(j, "searchVisible", !lv_obj_has_flag(b->search, LV_OBJ_FLAG_HIDDEN));
    cJSON_AddStringToObject(j, "query", lv_textarea_get_text(b->input));
    cJSON_AddStringToObject(j, "searchStatus", lv_label_get_text(b->search_hint));
    cJSON_AddStringToObject(j, "resultSymbol", cf_str(b->search_stock, "symbol"));
    cJSON_AddStringToObject(j, "selectedSymbol", cf_str(card_at(b, b->selected), "symbol"));
    cJSON_AddNumberToObject(j, "listScrollY", lv_obj_get_scroll_y(b->list));
    return j;
}

#include "clip_reader_board.h"
#include "clip_feed_widgets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INK 0xE8EEEE
#define MUTED 0x91A0A5
#define BG 0x13191C
#define CARD 0x1C2428
#define BLUE 0x247CCA
typedef struct {
    struct clip_reader_board_t *board;
    int index;
} reader_tap_t;
struct clip_reader_board_t {
    clip_fonts_t fonts;
    lv_obj_t *root, *content, *sheet, *status;
    bool weather, refresh;
    int selected, tab;
    cJSON *payload, *city_results;
    char *raw, *action_request;
    reader_tap_t taps[10], tabs[3], region_taps[8], city_taps[8], pin_taps[8];
    lv_obj_t *detail_body, *regions, *region_list, *region_notice, *search, *input, *keyboard, *results,
        *search_hint;
    char weather_id[24];
    uint32_t action_id, handled_id;
    int navigation;
    bool reset_detail;
};
static void news_detail(clip_reader_board_handle_t b);
static void regions_render(clip_reader_board_handle_t b);
static void weather_render(clip_reader_board_handle_t b);
static lv_obj_t *wrapped(clip_reader_board_handle_t b, lv_obj_t *p, int x, int y, int w, const char *text,
                         int f, uint32_t color) {
    lv_obj_t *o = cf_label(&b->fonts, p, x, y, w, text, f, color);
    cf_wrap(o);
    lv_obj_update_layout(o);
    return o;
}
static const char *condition(int code) {
    if (code == 0) {
        return "晴";
    }
    if (code <= 3) {
        return "局部多云";
    }
    if (code == 45 || code == 48) {
        return "雾";
    }
    if (code >= 95) {
        return "雷雨";
    }
    if (code >= 71 && code <= 77) {
        return "雪";
    }
    if (code >= 51) {
        return "雨";
    }
    return "多云";
}
static void weather_icon(lv_obj_t *p, int x, int y, int code) {
    if (code < 4) {
        cf_box(p, x + 18, y, 25, 25, 0xFFD861, 13);
    }
    if (code != 0) {
        cf_box(p, x + 3, y + 13, 35, 15, 0xF3F8FF, 8);
        cf_box(p, x + 7, y + 6, 19, 20, 0xF3F8FF, 10);
        cf_box(p, x + 20, y + 10, 20, 17, 0xF3F8FF, 10);
    }
    if (code >= 51) {
        cf_box(p, x + 10, y + 33, 3, 7, 0x8DCCFF, 2);
        cf_box(p, x + 24, y + 33, 3, 7, 0x8DCCFF, 2);
    }
}
static lv_obj_t *glass(clip_reader_board_handle_t b, int y, int h) {
    lv_obj_t *o = cf_box(b->content, 18, y, 444, h, 0x226CB3, 18);
    lv_obj_set_style_bg_opa(o, 95, 0);
    return o;
}
static void centered(clip_reader_board_handle_t b, int y, const char *text, int f) {
    lv_obj_t *o = cf_label(&b->fonts, b->content, 18, y, 444, text, f, 0xFFFFFF);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}
static const cJSON *weather_view(clip_reader_board_handle_t b) {
    const cJSON *locations = cJSON_GetObjectItemCaseSensitive(b->payload, "locations");
    if (!*b->weather_id) {
        snprintf(b->weather_id, sizeof(b->weather_id), "%s", cf_str(b->payload, "defaultId"));
    }
    const cJSON *loc;
    cJSON_ArrayForEach(loc, locations) if (!strcmp(cf_str(loc, "id"), b->weather_id)) {
        const cJSON *forecast = cJSON_GetObjectItemCaseSensitive(loc, "forecast");
        if (forecast) {
            return forecast;
        }
    }
    return b->payload;
}
static void weather_render(clip_reader_board_handle_t b) {
    int32_t scroll = lv_obj_get_scroll_y(b->content);
    lv_obj_clean(b->content);
    const cJSON *view = weather_view(b);
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(view, "current"),
                *daily = cJSON_GetObjectItemCaseSensitive(view, "daily"),
                *hourly = cJSON_GetObjectItemCaseSensitive(view, "hourly");
    const char *city = cf_str(view, "city");
    centered(b, 75, *city ? city : "天气", 3);
    char *text = malloc(160);
    if (!text) {
        return;
    }
    double temp = cf_num(current, "temperature_2m"), code = cf_num(current, "weather_code");
    if (isfinite(temp)) {
        snprintf(text, 160, "%.0f°", temp);
    } else {
        snprintf(text, 160, "--°");
    }
    centered(b, 106, text, 5);
    const cJSON *high = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"),
                *low = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"),
                *codes = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    const cJSON *hi = cJSON_GetArrayItem(high, 0), *lo = cJSON_GetArrayItem(low, 0);
    if (cJSON_IsNumber(hi) && cJSON_IsNumber(lo)) {
        snprintf(text, 160, "最高 %.0f°   最低 %.0f°", hi->valuedouble, lo->valuedouble);
    } else {
        snprintf(text, 160, "等待天气预报");
    }
    centered(b, 227, text, 2);
    centered(b, 265, isfinite(code) ? condition((int)code) : "等待联网更新", 2);
    double fetched = cf_num(view, "fetchedAt");
    if (isfinite(fetched)) {
        uint64_t seconds = (uint64_t)(fetched / 1000) + 28800;
        snprintf(text, 160, "更新 %02u:%02u · Wi-Fi", (unsigned)(seconds / 3600 % 24),
                 (unsigned)(seconds / 60 % 60));
        centered(b, 302, text, 0);
    }
    lv_obj_t *air = glass(b, 329, 100);
    double aqi = cf_num(cJSON_GetObjectItemCaseSensitive(view, "airQuality"), "us_aqi");
    if (isfinite(aqi)) {
        snprintf(text, 160, "%.0f · %s", aqi,
                 aqi <= 50    ? "优"
                 : aqi <= 100 ? "良"
                 : aqi <= 150 ? "轻度污染"
                              : "污染");
    } else {
        snprintf(text, 160, "空气质量");
    }
    cf_label(&b->fonts, air, 18, 12, 400, text, 2, 0xFFFFFF);
    const uint32_t colors[] = {0x39D87D, 0xDCE846, 0xFFAB38, 0xF54456, 0xAF4BE2, 0x8B2042};
    for (int i = 0; i < 6; i++) {
        cf_box(air, 18 + i * 68, 54, 69, 4, colors[i], 2);
    }
    if (isfinite(aqi)) {
        int x = 18 + (int)fmin(408, fmax(0, aqi / 300 * 408));
        lv_obj_t *dot = cf_box(air, x, 51, 9, 9, 0xFFFFFF, 5);
        lv_obj_set_style_border_width(dot, 1, 0);
    }
    cf_label(&b->fonts, air, 18, 70, 408,
             isfinite(aqi) ? "AQI (US) · Open-Meteo 模型估算" : "空气质量暂不可用", 0, 0xDAECFF);
    lv_obj_t *hours = glass(b, 441, 160);
    cf_label(&b->fonts, hours, 18, 12, 408, "每小时天气预报", 1, 0xE3F3FF);
    cf_box(hours, 18, 42, 408, 1, 0x6BA5D8, 0);
    lv_obj_t *strip = cf_box(hours, 12, 51, 420, 100, BLUE, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    cf_scroll(strip, LV_DIR_HOR);
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time"),
                *temps = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m"),
                *hour_codes = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
    int n = cJSON_GetArraySize(times);
    for (int i = 0; i < n && i < 24; i++) {
        const cJSON *time = cJSON_GetArrayItem(times, i), *t = cJSON_GetArrayItem(temps, i),
                    *c = cJSON_GetArrayItem(hour_codes, i);
        if (!cJSON_IsString(time) || !cJSON_IsNumber(t)) {
            continue;
        }
        if (strlen(time->valuestring) >= 13) {
            snprintf(text, 160, "%.2s时", time->valuestring + 11);
        } else {
            snprintf(text, 160, "--");
        }
        cf_label(&b->fonts, strip, i * 68 + 6, 0, 64, text, 0, 0xFFFFFF);
        weather_icon(strip, i * 68 + 8, 30, cJSON_IsNumber(c) ? c->valueint : 3);
        snprintf(text, 160, "%.0f°", t->valuedouble);
        cf_label(&b->fonts, strip, i * 68 + 10, 75, 58, text, 1, 0xFFFFFF);
    }
    if (!n) {
        cf_label(&b->fonts, strip, 8, 23, 400, "等待逐小时预报更新", 1, 0xFFFFFF);
    }
    int days = cJSON_GetArraySize(high);
    if (days > 10) {
        days = 10;
    }
    lv_obj_t *forecast = glass(b, 613, 53 + days * 58);
    cf_label(&b->fonts, forecast, 18, 13, 408, "10 日天气预报", 1, 0xC0DDF7);
    double minimum = INFINITY, maximum = -INFINITY;
    for (int i = 0; i < days; i++) {
        const cJSON *l = cJSON_GetArrayItem(low, i), *h = cJSON_GetArrayItem(high, i);
        if (cJSON_IsNumber(l) && cJSON_IsNumber(h)) {
            minimum = fmin(minimum, l->valuedouble);
            maximum = fmax(maximum, h->valuedouble);
        }
    }
    const cJSON *dates = cJSON_GetObjectItemCaseSensitive(daily, "time");
    for (int i = 0; i < days; i++) {
        int y = 46 + i * 58;
        const cJSON *l = cJSON_GetArrayItem(low, i), *h = cJSON_GetArrayItem(high, i),
                    *c = cJSON_GetArrayItem(codes, i), *date = cJSON_GetArrayItem(dates, i);
        cf_box(forecast, 18, y, 408, 1, 0x5E9DD0, 0);
        if (i == 0) {
            snprintf(text, 160, "今天");
        } else if (cJSON_IsString(date) && strlen(date->valuestring) == 10) {
            snprintf(text, 160, "%.2s/%.2s", date->valuestring + 5, date->valuestring + 8);
        } else {
            snprintf(text, 160, "--");
        }
        cf_label(&b->fonts, forecast, 18, y + 16, 85, text, 1, 0xFFFFFF);
        weather_icon(forecast, 109, y + 11, cJSON_IsNumber(c) ? c->valueint : 3);
        if (cJSON_IsNumber(l) && cJSON_IsNumber(h)) {
            snprintf(text, 160, "%.0f°", l->valuedouble);
            cf_label(&b->fonts, forecast, 175, y + 16, 62, text, 1, 0xACD2F2);
            snprintf(text, 160, "%.0f°", h->valuedouble);
            cf_label(&b->fonts, forecast, 363, y + 16, 65, text, 1, 0xFFFFFF);
            cf_box(forecast, 238, y + 26, 112, 5, 0x468AC0, 3);
            double span = fmax(1, maximum - minimum);
            int start = (int)((l->valuedouble - minimum) / span * 112),
                width = (int)((h->valuedouble - l->valuedouble) / span * 112);
            if (width < 4) {
                width = 4;
            }
            cf_box(forecast, 238 + start, y + 26, width, 5, 0xFFB542, 3);
        }
    }
    int y = 681 + days * 58;
    lv_obj_t *extra = glass(b, y, 120);
    double feels = cf_num(current, "apparent_temperature"), wind = cf_num(current, "wind_speed_10m"),
           humidity = cf_num(current, "relative_humidity_2m");
    if (isfinite(feels) && isfinite(wind) && isfinite(humidity)) {
        snprintf(text, 160, "体感 %.0f°   湿度 %.0f%%\n风速 %.1f km/h", feels, humidity, wind);
    } else {
        snprintf(text, 160, "等待天气数据更新");
    }
    wrapped(b, extra, 18, 18, 408, text, 1, 0xFFFFFF);
    b->status = wrapped(b, b->content, 24, y + 140, 432, "", 0, 0xE0F1FF);
    cf_label(&b->fonts, b->content, 24, y + 180, 432, "预报结束 · 上下滑动查看完整10天", 0, 0xE0F1FF);
    lv_obj_set_height(b->content, 800);
    lv_obj_scroll_to_y(b->content, scroll, LV_ANIM_OFF);
    free(text);
}
static void close_detail(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_add_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
}
static void refresh(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    b->refresh = true;
}
static void news_tap(lv_event_t *e) {
    reader_tap_t *t = lv_event_get_user_data(e);
    t->board->selected = t->index;
    t->board->tab = 0;
    t->board->reset_detail = true;
    news_detail(t->board);
    lv_obj_remove_flag(t->board->sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(t->board->sheet);
}
static void news_tab(lv_event_t *e) {
    reader_tap_t *t = lv_event_get_user_data(e);
    t->board->tab = t->index;
    t->board->reset_detail = true;
    news_detail(t->board);
}
static void news_render(clip_reader_board_handle_t b) {
    int32_t scroll = lv_obj_get_scroll_y(b->content);
    lv_obj_clean(b->content);
    cf_label(&b->fonts, b->content, 20, 19, 310, "AIHOT", 4, INK);
    cf_label(&b->fonts, b->content, 21, 76, 390, "AI 热点榜 · 前十条", 1, MUTED);
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(b->payload, "cards");
    int n = cJSON_GetArraySize(cards);
    int y = 126;
    for (int i = 0; i < n && i < 10; i++) {
        const cJSON *c = cJSON_GetArrayItem(cards, i);
        lv_obj_t *row = cf_box(b->content, 18, y, 444, 146, CARD, 16);
        b->taps[i] = (reader_tap_t){b, i};
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, news_tap, LV_EVENT_CLICKED, &b->taps[i]);
        char rank[12];
        snprintf(rank, sizeof(rank), "%02d", i + 1);
        cf_label(&b->fonts, row, 16, 15, 42, rank, 2, i < 3 ? 0xF68162 : MUTED);
        lv_obj_t *title = wrapped(
            b, row, 64, 14, 361, *cf_str(c, "headline") ? cf_str(c, "headline") : cf_str(c, "value"), 2, INK);
        lv_obj_set_height(title, 80);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        char *meta = malloc(160);
        if (meta) {
            snprintf(meta, 160, "%.0f 个来源 · 点开阅读  ›", cf_num(c, "sourceCount"));
            cf_label(&b->fonts, row, 64, 115, 361, meta, 0, MUTED);
            free(meta);
        }
        y += 158;
    }
    if (!n) {
        wrapped(b, b->content, 24, y, 432, "等待联网获取热榜。连接背夹 Wi-Fi 或手机后更新。", 2, MUTED);
    }
    b->status = wrapped(b, b->content, 24, y + 18, 432, "", 0, MUTED);
    lv_obj_update_layout(b->content);
    lv_obj_scroll_to_y(b->content, scroll, LV_ANIM_OFF);
    if (!lv_obj_has_flag(b->sheet, LV_OBJ_FLAG_HIDDEN)) {
        news_detail(b);
    }
}
static void news_previous(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    b->navigation = -1;
}
static void news_next(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    b->navigation = 1;
}
static void news_gesture(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
        b->navigation = direction == LV_DIR_LEFT ? 1 : -1;
        lv_indev_wait_release(indev);
    }
}
static void news_detail(clip_reader_board_handle_t b) {
    int32_t old_scroll = b->detail_body && !b->reset_detail ? lv_obj_get_scroll_y(b->detail_body) : 0;
    b->reset_detail = false;
    lv_obj_clean(b->sheet);
    b->detail_body = NULL;
    cf_button(&b->fonts, b->sheet, 18, 14, 80, 40, "‹ 返回", 0x293337, close_detail, b);
    cf_button(&b->fonts, b->sheet, 382, 14, 80, 40, "刷新", 0x293337, refresh, b);
    int count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b->payload, "cards"));
    cf_button(&b->fonts, b->sheet, 110, 14, 48, 40, "‹", 0x293337, news_previous, b);
    lv_obj_t *counter = cf_label(&b->fonts, b->sheet, 168, 24, 100, "", 1, MUTED);
    lv_label_set_text_fmt(counter, "%d / %d", b->selected + 1, count);
    cf_button(&b->fonts, b->sheet, 278, 14, 48, 40, "›", 0x293337, news_next, b);
    cf_label(&b->fonts, b->sheet, 22, 770, 436, "左右滑动切篇 · 上下滑动阅读全文", 0, MUTED);
    lv_obj_t *body = cf_box(b->sheet, 0, 73, 480, 685, BG, 0);
    b->detail_body = body;
    cf_scroll(body, LV_DIR_VER);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(body, news_gesture, LV_EVENT_GESTURE, b);
    const cJSON *c = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(b->payload, "cards"), b->selected);
    const char *headline = *cf_str(c, "headline") ? cf_str(c, "headline") : cf_str(c, "value");
    lv_obj_t *title = wrapped(b, body, 20, 12, 440, headline, 3, INK);
    int y = lv_obj_get_height(title) + 42;
    char *text = malloc(768);
    if (!text) {
        return;
    }
    double reports = cf_num(c, "reportCount"), sources = cf_num(c, "sourceCount");
    if (isfinite(reports)) {
        snprintf(text, 768, "%.0f 篇报道   ·   %.0f 个来源", reports, sources);
    } else {
        snprintf(text, 768, "%.0f 个来源 · AIHOT 热榜第 %d 名", sources, b->selected + 1);
    }
    cf_label(&b->fonts, body, 20, y, 440, text, 0, MUTED);
    y += 30;
    const char *latest = *cf_str(c, "updatedAt") ? cf_str(c, "updatedAt") : cf_str(c, "latestAt");
    if (*latest) {
        snprintf(text, 768, "更新 %.*s (UTC)", 19, latest);
        cf_label(&b->fonts, body, 20, y, 440, text, 0, MUTED);
        y += 35;
    }
    const char *tabs[] = {"事件概览", "来源信息", "热度数据"};
    for (int i = 0; i < 3; i++) {
        b->tabs[i] = (reader_tap_t){b, i};
        cf_button(&b->fonts, body, 20 + i * 148, y, 140, 42, tabs[i], i == b->tab ? 0x344248 : 0x1C2428,
                  news_tab, &b->tabs[i]);
    }
    y += 64;
    lv_obj_t *panel = cf_box(body, 18, y, 444, 160, CARD, 18);
    const char *heading = b->tab == 0 ? "先了解这件事" : b->tab == 1 ? "报道来源" : "为什么热";
    cf_label(&b->fonts, panel, 20, 18, 395, heading, 2, INK);
    const char *content = NULL;
    if (b->tab == 0) {
        content = cf_str(c, "body");
        if (!*content) {
            content = "事件正文暂未获取。点击右上角刷新后重试，当前保留热榜标题和来源。";
        }
    } else if (b->tab == 1) {
        snprintf(text, 768, "%s\n\n原始来源\n%s\n\n事件页面\n%s", cf_str(c, "detail"),
                 cf_str(c, "originalUrl"), cf_str(c, "storyUrl"));
        content = text;
    } else {
        double signals = cf_num(c, "signalCount"), participants = cf_num(c, "participantCount");
        snprintf(text, 768,
                 "当前热榜第 %d 名\n\n%.0f 个报道来源\n%.0f 条热度信号\n%.0f 个参与账号\n\n统计来自 "
                 "AIHOT，随网站更新。",
                 b->selected + 1, isfinite(sources) ? sources : 0, isfinite(signals) ? signals : 0,
                 isfinite(participants) ? participants : 0);
        content = text;
    }
    lv_obj_t *article = wrapped(b, panel, 20, 65, 404, content, 2, 0xCAD3D6);
    int h = lv_obj_get_height(article) + 92;
    lv_obj_set_height(panel, h);
    y += h + 22;
    wrapped(b, body, 24, y, 432, "正文结束 · AIHOT 事件综述\n左右切换上一篇 / 下一篇", 0, MUTED);
    lv_obj_update_layout(body);
    lv_obj_scroll_to_y(body, old_scroll, LV_ANIM_OFF);
    free(text);
}
static void regions_close(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_add_flag(b->regions, LV_OBJ_FLAG_HIDDEN);
}
static void search_close(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_add_flag(b->search, LV_OBJ_FLAG_HIDDEN);
}
static void region_choose(lv_event_t *e) {
    reader_tap_t *tap = lv_event_get_user_data(e);
    clip_reader_board_handle_t b = tap->board;
    const cJSON *loc =
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(b->payload, "locations"), tap->index);
    snprintf(b->weather_id, sizeof(b->weather_id), "%s", cf_str(loc, "id"));
    lv_obj_add_flag(b->regions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(b->content, 0, LV_ANIM_OFF);
    weather_render(b);
}
static void region_pin(lv_event_t *e) {
    reader_tap_t *tap = lv_event_get_user_data(e);
    clip_reader_board_handle_t b = tap->board;
    const cJSON *loc =
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(b->payload, "locations"), tap->index);
    snprintf(b->weather_id, sizeof(b->weather_id), "%s", cf_str(loc, "id"));
    cJSON *id = cJSON_CreateString(b->weather_id);
    cf_action(&b->action_request, &b->action_id, "weather_pin", NULL, "id", id);
    cJSON_Delete(id);
    lv_label_set_text(b->region_notice, "正在保存默认显示地区…");
}
static void city_pick(lv_event_t *e) {
    reader_tap_t *tap = lv_event_get_user_data(e);
    clip_reader_board_handle_t b = tap->board;
    const cJSON *city = cJSON_GetArrayItem(b->city_results, tap->index);
    cf_action(&b->action_request, &b->action_id, "weather_add", NULL, "city", city);
    lv_label_set_text(b->search_hint, "正在通过背夹 Wi-Fi 获取该地区天气…");
}
static const char *common_cities[] = {"龙岗区", "紫阳县", "安康市", "高台县"};
static void city_query(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    const char *q = lv_textarea_get_text(b->input);
    if (strlen(q) < 2) {
        lv_label_set_text(b->search_hint, "请输入城市名或拼音");
        return;
    }
    cf_action(&b->action_request, &b->action_id, "city_search", q, NULL, NULL);
    lv_obj_clean(b->results);
    lv_label_set_text(b->search_hint, "正在查询地区…");
}
static void common_pick(lv_event_t *e) {
    reader_tap_t *tap = lv_event_get_user_data(e);
    clip_reader_board_handle_t b = tap->board;
    lv_textarea_set_text(b->input, common_cities[tap->index]);
    cf_action(&b->action_request, &b->action_id, "city_search", common_cities[tap->index], NULL, NULL);
    lv_obj_clean(b->results);
    lv_label_set_text(b->search_hint, "正在查询地区…");
}
static void city_search_open(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    lv_obj_remove_flag(b->search, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(b->search);
    lv_textarea_set_text(b->input, "");
    lv_obj_clean(b->results);
    lv_label_set_text(b->search_hint, "输入城市名/拼音，或选择下方地区");
}
static void regions_render(clip_reader_board_handle_t b) {
    int32_t scroll = b->region_list ? lv_obj_get_scroll_y(b->region_list) : 0;
    lv_obj_clean(b->regions);
    b->region_list = NULL;
    cf_label(&b->fonts, b->regions, 20, 20, 220, "天气 · 地区", 3, 0xFFFFFF);
    cf_button(&b->fonts, b->regions, 272, 18, 86, 42, "添加", 0x397DB6, city_search_open, b);
    cf_button(&b->fonts, b->regions, 370, 18, 92, 42, "返回", 0x397DB6, regions_close, b);
    b->region_notice = cf_label(&b->fonts, b->regions, 20, 72, 440,
                                "设为默认后，打开天气 App 会优先显示该地区", 0, 0xC9E4F7);
    b->region_list = cf_box(b->regions, 18, 108, 444, 654, 0x225786, 0);
    lv_obj_set_style_bg_opa(b->region_list, LV_OPA_TRANSP, 0);
    cf_scroll(b->region_list, LV_DIR_VER);
    const cJSON *locations = cJSON_GetObjectItemCaseSensitive(b->payload, "locations");
    int n = cJSON_GetArraySize(locations);
    char *text = malloc(180);
    if (!text) {
        return;
    }
    for (int i = 0; i < n && i < 8; i++) {
        const cJSON *loc = cJSON_GetArrayItem(locations, i),
                    *forecast = cJSON_GetObjectItemCaseSensitive(loc, "forecast"),
                    *current = cJSON_GetObjectItemCaseSensitive(forecast, "current"),
                    *daily = cJSON_GetObjectItemCaseSensitive(forecast, "daily");
        lv_obj_t *card = cf_box(b->region_list, 0, i * 150, 444, 140, 0x4F8FB9, 18);
        b->region_taps[i] = (reader_tap_t){b, i};
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, region_choose, LV_EVENT_CLICKED, &b->region_taps[i]);
        cf_label(&b->fonts, card, 16, 12, 260, cf_str(loc, "name"), 3, 0xFFFFFF);
        double t = cf_num(current, "temperature_2m");
        snprintf(text, 180, isfinite(t) ? "%.0f°" : "--°", t);
        lv_obj_t *temp = cf_label(&b->fonts, card, 285, 8, 145, text, 4, 0xFFFFFF);
        lv_obj_set_style_text_align(temp, LV_TEXT_ALIGN_RIGHT, 0);
        double code = cf_num(current, "weather_code");
        cf_label(&b->fonts, card, 16, 57, 230, isfinite(code) ? condition((int)code) : "等待 Wi-Fi 更新", 1,
                 0xE2F2FF);
        const cJSON *high =
                        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), 0),
                    *low =
                        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), 0);
        if (cJSON_IsNumber(high) && cJSON_IsNumber(low)) {
            snprintf(text, 180, "最高 %.0f°  最低 %.0f°", high->valuedouble, low->valuedouble);
            cf_label(&b->fonts, card, 208, 65, 220, text, 0, 0xFFFFFF);
        }
        bool pinned = !strcmp(cf_str(loc, "id"), cf_str(b->payload, "defaultId"));
        if (pinned) {
            cf_label(&b->fonts, card, 16, 104, 240, "默认显示地区", 1, 0xFFFFFF);
        } else {
            b->pin_taps[i] = (reader_tap_t){b, i};
            cf_button(&b->fonts, card, 16, 99, 152, 32, "设为默认显示", 0x326C97, region_pin,
                      &b->pin_taps[i]);
        }
    }
    if (!n) {
        cf_label(&b->fonts, b->region_list, 12, 20, 420, "正在通过 Wi-Fi 载入地区…", 1, 0xFFFFFF);
    }
    cf_label(&b->fonts, b->regions, 20, 774, 440, "地区 DataV / GeoNames · 天气 Open-Meteo", 0, 0xBBD5E9);
    lv_obj_update_layout(b->region_list);
    lv_obj_scroll_to_y(b->region_list, scroll, LV_ANIM_OFF);
    free(text);
}
static void regions_open(lv_event_t *e) {
    clip_reader_board_handle_t b = lv_event_get_user_data(e);
    regions_render(b);
    lv_obj_remove_flag(b->regions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(b->regions);
}
static void weather_panels_create(clip_reader_board_handle_t b) {
    b->regions = cf_box(b->root, 0, 0, 480, 800, 0x225786, 0);
    lv_obj_add_flag(b->regions, LV_OBJ_FLAG_HIDDEN);
    b->search = cf_box(b->root, 0, 0, 480, 800, 0x225786, 0);
    cf_label(&b->fonts, b->search, 20, 20, 290, "添加天气地区", 3, 0xFFFFFF);
    cf_button(&b->fonts, b->search, 370, 18, 92, 42, "返回", 0x397DB6, search_close, b);
    b->input = lv_textarea_create(b->search);
    lv_obj_set_pos(b->input, 18, 78);
    lv_obj_set_size(b->input, 330, 52);
    lv_textarea_set_one_line(b->input, true);
    lv_textarea_set_max_length(b->input, 40);
    lv_obj_set_style_text_font(b->input, b->fonts.fonts[1], 0);
    lv_textarea_set_placeholder_text(b->input, "城市名 / 拼音");
    lv_obj_add_event_cb(b->input, city_query, LV_EVENT_READY, b);
    cf_button(&b->fonts, b->search, 362, 78, 100, 52, "搜索", 0x397DB6, city_query, b);
    b->search_hint =
        cf_label(&b->fonts, b->search, 18, 144, 444, "输入城市名/拼音，或选择下方地区", 0, 0xD1E9F9);
    for (int i = 0; i < 4; i++) {
        b->taps[i] = (reader_tap_t){b, i};
        cf_button(&b->fonts, b->search, 18 + (i % 2) * 226, 180 + (i / 2) * 44, 216, 38, common_cities[i],
                  0x397DB6, common_pick, &b->taps[i]);
    }
    b->results = cf_box(b->search, 18, 278, 444, 260, 0x225786, 0);
    cf_scroll(b->results, LV_DIR_VER);
    b->keyboard = lv_keyboard_create(b->search);
    lv_obj_set_size(b->keyboard, 480, 250);
    lv_obj_align(b->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(b->keyboard, b->input);
    lv_obj_add_flag(b->search, LV_OBJ_FLAG_HIDDEN);
}

clip_reader_board_handle_t clip_reader_board_create(lv_obj_t *screen, bool weather) {
    clip_reader_board_handle_t b = calloc(1, sizeof(*b));
    if (!b) {
        return NULL;
    }
    cf_load(&b->fonts);
    b->weather = weather;
    b->root = cf_box(screen, 0, 0, 480, 800, weather ? 0x5498D5 : BG, 0);
    if (weather) {
        lv_obj_set_style_bg_grad_color(b->root, lv_color_hex(0x2676BB), 0);
        lv_obj_set_style_bg_grad_dir(b->root, LV_GRAD_DIR_VER, 0);
        for (int i = 0; i < 8; i++) {
            lv_obj_t *cloud = cf_box(b->root, -40 + i * 65, 25 + (i % 3) * 18, 160, 95, 0xF1F6FC, 70);
            lv_obj_set_style_bg_opa(cloud, 25 + (i % 3) * 7, 0);
        }
    }
    b->content = cf_box(b->root, 0, 0, 480, 800, 0, 0);
    lv_obj_set_style_bg_opa(b->content, LV_OPA_TRANSP, 0);
    cf_scroll(b->content, LV_DIR_VER);
    b->sheet = cf_box(b->root, 0, 0, 480, 800, BG, 0);
    lv_obj_add_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
    if (weather) {
        weather_panels_create(b);
        weather_render(b);
        cf_button(&b->fonts, b->root, 18, 18, 90, 40, "地区", 0x397DB6, regions_open, b);
        cf_button(&b->fonts, b->root, 378, 18, 84, 40, "刷新", 0x397DB6, refresh, b);
    } else {
        news_render(b);
        cf_button(&b->fonts, b->root, 378, 26, 84, 40, "刷新", 0x293337, refresh, b);
    }
    return b;
}
void clip_reader_board_delete(clip_reader_board_handle_t b) {
    if (!b) {
        return;
    }
    lv_obj_delete(b->root);
    cJSON_Delete(b->payload);
    cJSON_Delete(b->city_results);
    free(b->action_request);
    free(b->raw);
    cf_free(&b->fonts);
    free(b);
}
void clip_reader_board_update(clip_reader_board_handle_t b, const char *raw, const char *status) {
    if (!b) {
        return;
    }
    if (raw && *raw && (!b->raw || strcmp(b->raw, raw))) {
        cJSON *p = cJSON_Parse(raw);
        if (p) {
            char *copy = strdup(raw);
            if (copy) {
                cJSON_Delete(b->payload);
                free(b->raw);
                b->payload = p;
                p = NULL;
                b->raw = copy;
                if (b->weather) {
                    weather_render(b);
                    if (b->regions && !lv_obj_has_flag(b->regions, LV_OBJ_FLAG_HIDDEN)) {
                        regions_render(b);
                    }
                } else {
                    news_render(b);
                }
            }
        }
        cJSON_Delete(p);
    }
    if (status && b->status) {
        lv_label_set_text(b->status, status);
    }
    if (!b->weather && b->navigation) {
        int n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b->payload, "cards"));
        if (n) {
            b->selected = (b->selected + n + b->navigation) % n;
            b->tab = 0;
            b->reset_detail = true;
            news_detail(b);
            lv_obj_remove_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
        }
        b->navigation = 0;
    }
}
void clip_reader_board_key(clip_reader_board_handle_t b, const char *key) {
    if (!b || !key) {
        return;
    }
    if (!strcmp(key, "confirm")) {
        if (b->weather) {
            b->refresh = true;
        } else if (!lv_obj_has_flag(b->sheet, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
        } else {
            b->reset_detail = true;
            news_detail(b);
            lv_obj_remove_flag(b->sheet, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (!b->weather && !lv_obj_has_flag(b->sheet, LV_OBJ_FLAG_HIDDEN)) {
        b->navigation = !strcmp(key, "left") ? -1 : 1;
    } else if (b->weather) {
        int32_t target = lv_obj_get_scroll_y(b->content) + (!strcmp(key, "left") ? -350 : 350);
        lv_obj_scroll_to_y(b->content, target < 0 ? 0 : target, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_by(b->content, 0, !strcmp(key, "left") ? 350 : -350, LV_ANIM_OFF);
    }
}
bool clip_reader_board_refresh_requested(clip_reader_board_handle_t b) {
    bool r = b && b->refresh;
    if (b) {
        b->refresh = false;
    }
    return r;
}

char *clip_reader_board_take_action(clip_reader_board_handle_t b) {
    if (!b) {
        return NULL;
    }
    char *r = b->action_request;
    b->action_request = NULL;
    return r;
}
void clip_reader_board_action_result(clip_reader_board_handle_t b, const char *raw) {
    if (!b || !b->weather || !raw) {
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
        if (b->search_hint) {
            lv_label_set_text(b->search_hint, cf_str(j, "error"));
        }
        if (b->region_notice) {
            lv_label_set_text(b->region_notice, cf_str(j, "error"));
        }
        cJSON_Delete(j);
        return;
    }
    if (!strcmp(cf_str(j, "op"), "city_search")) {
        cJSON_Delete(b->city_results);
        b->city_results = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "cities"), true);
        lv_obj_clean(b->results);
        int n = cJSON_GetArraySize(b->city_results);
        for (int i = 0; i < n && i < 8; i++) {
            const cJSON *c = cJSON_GetArrayItem(b->city_results, i);
            lv_obj_t *row = cf_box(b->results, 0, i * 82, 444, 78, 0x286A9E, 12);
            b->city_taps[i] = (reader_tap_t){b, i};
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, city_pick, LV_EVENT_CLICKED, &b->city_taps[i]);
            cf_label(&b->fonts, row, 14, 9, 414, cf_str(c, "name"), 2, 0xFFFFFF);
            cf_label(&b->fonts, row, 14, 45, 414, cf_str(c, "admin1"), 0, 0xCCE8FF);
        }
        lv_label_set_text(b->search_hint, "点选地区，背夹将通过 Wi-Fi 获取天气");
    } else if (!strcmp(cf_str(j, "op"), "weather_add")) {
        snprintf(b->weather_id, sizeof(b->weather_id), "%s", cf_str(j, "locationId"));
        lv_obj_add_flag(b->search, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(b->regions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(b->content, 0, LV_ANIM_OFF);
        weather_render(b);
    } else if (!strcmp(cf_str(j, "op"), "weather_pin")) {
        lv_obj_add_flag(b->regions, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(b->content, 0, LV_ANIM_OFF);
        weather_render(b);
    }
    cJSON_Delete(j);
}

cJSON *clip_reader_board_inspect(clip_reader_board_handle_t b) {
    if (!b) {
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    lv_obj_t *scroller = b->weather || lv_obj_has_flag(b->sheet, LV_OBJ_FLAG_HIDDEN)
                             ? b->content : b->detail_body;
    cJSON_AddStringToObject(j, "view", b->weather ? "weather" : "aihot");
    cJSON_AddNumberToObject(j, "contentChildren", lv_obj_get_child_count(b->content));
    if (scroller) {
        cJSON_AddNumberToObject(j, "scrollY", lv_obj_get_scroll_y(scroller));
        cJSON_AddNumberToObject(j, "scrollMax",
                                lv_obj_get_scroll_y(scroller) + lv_obj_get_scroll_bottom(scroller));
    }
    if (b->weather) {
        const cJSON *view = weather_view(b);
        cJSON_AddStringToObject(j, "locationId", b->weather_id);
        cJSON_AddStringToObject(j, "city", cf_str(view, "city"));
        cJSON_AddStringToObject(j, "defaultId", cf_str(b->payload, "defaultId"));
        cJSON_AddNumberToObject(
            j, "locations", cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b->payload, "locations")));
        cJSON_AddNumberToObject(j, "days",
                                cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(
                                    cJSON_GetObjectItemCaseSensitive(view, "daily"), "time")));
        cJSON_AddBoolToObject(j, "regionsVisible",
                              b->regions && !lv_obj_has_flag(b->regions, LV_OBJ_FLAG_HIDDEN));
        cJSON_AddBoolToObject(j, "searchVisible",
                              b->search && !lv_obj_has_flag(b->search, LV_OBJ_FLAG_HIDDEN));
    } else {
        const cJSON *c =
            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(b->payload, "cards"), b->selected);
        cJSON_AddNumberToObject(j, "articleIndex", b->selected + 1);
        cJSON_AddNumberToObject(j, "articleCount",
                                cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(b->payload, "cards")));
        cJSON_AddStringToObject(j, "headline", cf_str(c, "headline"));
        cJSON_AddNumberToObject(j, "bodyBytes", strlen(cf_str(c, "body")));
        int paragraphs = 1;
        const char *text = cf_str(c, "body");
        for (const char *p = text; *p; p++) {
            if (p[0] == '\n' && p[1] == '\n') {
                paragraphs++;
            }
        }
        cJSON_AddNumberToObject(j, "paragraphs", paragraphs);
    }
    return j;
}

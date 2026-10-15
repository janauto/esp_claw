#include "clip_live_format.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *str(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool number(const cJSON *j, const char *key, double *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}
static char *format(const char *pattern, ...) {
    va_list args, copy;
    va_start(args, pattern);
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, pattern, copy);
    va_end(copy);
    char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (text) {
        vsnprintf(text, (size_t)n + 1, pattern, args);
    }
    va_end(args);
    return text;
}
static char *short_utf8(const char *text, size_t limit) {
    size_t size = strlen(text);
    if (size > limit) {
        size = limit;
        while (size && (((unsigned char)text[size] & 0xC0) == 0x80)) {
            size--;
        }
    }
    char *out = malloc(size + 1);
    if (out) {
        memcpy(out, text, size);
        out[size] = 0;
    }
    return out;
}
static cJSON *card(const char *title, const char *value, const char *detail, const char *url) {
    if (!title || !value || !detail) {
        return NULL;
    }
    cJSON *c = cJSON_CreateObject();
    if (!c) {
        return NULL;
    }
    cJSON_AddStringToObject(c, "title", title);
    cJSON_AddStringToObject(c, "value", value);
    cJSON_AddStringToObject(c, "detail", detail);
    cJSON_AddStringToObject(c, "url", url ? url : "");
    return c;
}
void clip_live_settings_defaults(clip_live_settings_t *s) {
    *s = (clip_live_settings_t){.city = "深圳",
                                .latitude = 22.54554,
                                .longitude = 114.0683,
                                .symbols = "us.DJI,usAAPL,usBA,usBRK.B,usDIS,usGE,usHD,usNKE,usSBUX,hk00941",
                                .mode = "auto"};
}
static bool symbols_valid(const char *s) {
    if (!s || !*s || strlen(s) >= 320) {
        return false;
    }
    unsigned count = 0;
    const char *p = s;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (++count > 20 || n < 3 || n > 14) {
            return false;
        }
        bool domestic =
            n == 8 && ((p[0] == 's' && (p[1] == 'h' || p[1] == 'z')) || (p[0] == 'b' && p[1] == 'j'));
        bool hk = n == 7 && p[0] == 'h' && p[1] == 'k';
        bool us = p[0] == 'u' && p[1] == 's';
        if (!domestic && !hk && !us) {
            return false;
        }
        for (size_t i = 2; i < n; i++) {
            if (us ? !(isupper((unsigned char)p[i]) || p[i] == '.') : !isdigit((unsigned char)p[i])) {
                return false;
            }
        }
        if (!end) {
            break;
        }
        p = end + 1;
        if (!*p) {
            return false;
        }
    }
    return true;
}
bool clip_live_settings_parse(const cJSON *json, clip_live_settings_t *s) {
    const char *city = str(json, "city"), *symbols = str(json, "symbols"), *mode = str(json, "mode");
    double lat, lon;
    if (!city || !*city || strlen(city) >= sizeof(s->city) || !symbols_valid(symbols) || !mode ||
        (strcmp(mode, "auto") && strcmp(mode, "wifi") && strcmp(mode, "phone")) ||
        !number(json, "latitude", &lat) || !number(json, "longitude", &lon) || lat < -90 || lat > 90 ||
        lon < -180 || lon > 180) {
        return false;
    }
    snprintf(s->city, sizeof(s->city), "%s", city);
    snprintf(s->symbols, sizeof(s->symbols), "%s", symbols);
    snprintf(s->mode, sizeof(s->mode), "%s", mode);
    s->latitude = lat;
    s->longitude = lon;
    return true;
}
cJSON *clip_live_settings_json(const clip_live_settings_t *s) {
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return NULL;
    }
    cJSON_AddStringToObject(j, "city", s->city);
    cJSON_AddStringToObject(j, "symbols", s->symbols);
    cJSON_AddStringToObject(j, "mode", s->mode);
    cJSON_AddNumberToObject(j, "latitude", s->latitude);
    cJSON_AddNumberToObject(j, "longitude", s->longitude);
    return j;
}
cJSON *clip_live_payload(const char *kind, cJSON *cards, uint64_t now) {
    if (!cards || cJSON_GetArraySize(cards) < 1) {
        cJSON_Delete(cards);
        return NULL;
    }
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        cJSON_Delete(cards);
        return NULL;
    }
    bool weather = !strcmp(kind, "weather"), stocks = !strcmp(kind, "stocks");
    cJSON_AddNumberToObject(j, "schemaVersion", 1);
    cJSON_AddStringToObject(j, "kind", kind);
    cJSON_AddNumberToObject(j, "fetchedAt", (double)now);
    cJSON_AddNumberToObject(j, "staleAfterMs", weather ? 1800000 : stocks ? 180000 : 900000);
    cJSON_AddStringToObject(j, "via", "wifi");
    cJSON_AddStringToObject(j, "source", weather ? "Open-Meteo" : stocks ? "腾讯财经" : "AIHOT");
    cJSON_AddStringToObject(j, "sourceUrl",
                            weather  ? "https://open-meteo.com/"
                            : stocks ? "https://gu.qq.com/"
                                     : "https://aihot.news/hot");
    cJSON_AddItemToObject(j, "cards", cards);
    return j;
}
static const char *weather_text(int code) {
    if (code == 0) {
        return "晴";
    }
    if (code >= 1 && code <= 3) {
        return "多云";
    }
    if (code == 45 || code == 48) {
        return "雾";
    }
    if (code >= 51 && code <= 57) {
        return "毛毛雨";
    }
    if (code >= 61 && code <= 67) {
        return "雨";
    }
    if (code >= 71 && code <= 77) {
        return "雪";
    }
    if (code >= 80 && code <= 82) {
        return "阵雨";
    }
    if (code == 85 || code == 86) {
        return "阵雪";
    }
    if (code >= 95 && code <= 99) {
        return "雷雨";
    }
    return "未知天气";
}
cJSON *clip_live_parse_weather(const cJSON *r, const clip_live_settings_t *s, uint64_t now) {
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(r, "current"),
                *daily = cJSON_GetObjectItemCaseSensitive(r, "daily");
    double temp, feel, humidity, wind, code;
    const char *time = str(current, "time");
    if (!time || !number(current, "temperature_2m", &temp) ||
        !number(current, "apparent_temperature", &feel) ||
        !number(current, "relative_humidity_2m", &humidity) || !number(current, "wind_speed_10m", &wind) ||
        !number(current, "weather_code", &code)) {
        return NULL;
    }
    cJSON *cards = cJSON_CreateArray();
    if (!cards) {
        return NULL;
    }
    char *title = format("%s · 当前天气", s->city),
         *value = format("%.1f°C  %s\n体感 %.1f°C\n湿度 %.0f%%  风速 %.1f km/h", temp,
                         weather_text((int)code), feel, humidity, wind),
         *detail = format("模型估算 · %s\nOpen-Meteo", time);
    cJSON *first = card(title, value, detail, "https://open-meteo.com/");
    free(title);
    free(value);
    free(detail);
    if (!first) {
        cJSON_Delete(cards);
        return NULL;
    }
    cJSON_AddItemToArray(cards, first);
    const cJSON *dates = cJSON_GetObjectItemCaseSensitive(daily, "time"),
                *high = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"),
                *low = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"),
                *rain = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    int count = cJSON_GetArraySize(dates);
    if (count < 1 || cJSON_GetArraySize(high) < count || cJSON_GetArraySize(low) < count ||
        cJSON_GetArraySize(rain) < count) {
        cJSON_Delete(cards);
        return NULL;
    }
    for (int i = 0; i < count && i < 3; i++) {
        const cJSON *date = cJSON_GetArrayItem(dates, i), *hi = cJSON_GetArrayItem(high, i),
                    *lo = cJSON_GetArrayItem(low, i), *pr = cJSON_GetArrayItem(rain, i);
        if (!cJSON_IsString(date) || !cJSON_IsNumber(hi) || !cJSON_IsNumber(lo) || !cJSON_IsNumber(pr)) {
            cJSON_Delete(cards);
            return NULL;
        }
        title = format("%s · %s", s->city, date->valuestring);
        value = format("%.1f ～ %.1f°C\n降水概率 %.0f%%", lo->valuedouble, hi->valuedouble, pr->valuedouble);
        cJSON *c = card(title, value, "每日预报 · Open-Meteo", "https://open-meteo.com/");
        free(title);
        free(value);
        if (!c) {
            cJSON_Delete(cards);
            return NULL;
        }
        cJSON_AddItemToArray(cards, c);
    }
    cJSON *payload = clip_live_payload("weather", cards, now);
    if (payload) {
        cJSON_AddStringToObject(payload, "city", s->city);
        cJSON_AddItemToObject(payload, "current", cJSON_Duplicate(current, true));
        cJSON_AddItemToObject(payload, "daily", cJSON_Duplicate(daily, true));
        const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(r, "hourly");
        if (hourly) {
            cJSON_AddItemToObject(payload, "hourly", cJSON_Duplicate(hourly, true));
        }
    }
    return payload;
}
cJSON *clip_live_parse_hot(const cJSON *r, uint64_t now) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(r, "schemaVersion"),
                *items = cJSON_GetObjectItemCaseSensitive(r, "items");
    int count = cJSON_GetArraySize(items);
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsArray(items) || count < 1 ||
        count > 10) {
        return NULL;
    }
    const cJSON *ranked[10] = {0}, *item;
    cJSON_ArrayForEach(item, items) {
        const cJSON *rank = cJSON_GetObjectItemCaseSensitive(item, "rank");
        if (!cJSON_IsNumber(rank) || rank->valueint < 1 || rank->valueint > 10 ||
            rank->valuedouble != rank->valueint || ranked[rank->valueint - 1]) {
            return NULL;
        }
        ranked[rank->valueint - 1] = item;
    }
    cJSON *cards = cJSON_CreateArray();
    if (!cards) {
        return NULL;
    }
    for (int i = 0; i < 10; i++) {
        if (ranked[i]) {
            item = ranked[i];
            const char *headline = str(item, "title"),
                       *name = str(cJSON_GetObjectItemCaseSensitive(item, "source"), "name"),
                       *url = str(cJSON_GetObjectItemCaseSensitive(item, "links"), "aihot");
            double sources;
            if (!headline || !*headline || strlen(headline) > 1200 || !name ||
                !number(item, "sourceCount", &sources)) {
                cJSON_Delete(cards);
                return NULL;
            }
            char *source_name = short_utf8(name, 120);
            if (!source_name) {
                cJSON_Delete(cards);
                return NULL;
            }
            char *title = format("AIHOT 热榜 · %d/%d", i + 1, count),
                 *detail = format("AIHOT · %.0f 个来源\n%s", sources, source_name);
            free(source_name);
            cJSON *c =
                card(title, headline, detail,
                     url && strncmp(url, "https://aihot.news/", 19) == 0 ? url : "https://aihot.news/hot");
            free(title);
            free(detail);
            if (!c) {
                cJSON_Delete(cards);
                return NULL;
            }
            cJSON_AddStringToObject(c, "headline", headline);
            cJSON_AddNumberToObject(c, "rank", i + 1);
            cJSON_AddNumberToObject(c, "sourceCount", sources);
            const char *story = str(cJSON_GetObjectItemCaseSensitive(item, "links"), "story"),
                       *original = str(cJSON_GetObjectItemCaseSensitive(item, "links"), "original");
            if (story) {
                cJSON_AddStringToObject(c, "storyUrl", story);
            }
            if (original) {
                cJSON_AddStringToObject(c, "originalUrl", original);
            }
            const char *latest = str(item, "latestAt");
            if (latest) {
                cJSON_AddStringToObject(c, "latestAt", latest);
            }
            double signals, participants;
            if (number(item, "signalCount", &signals)) {
                cJSON_AddNumberToObject(c, "signalCount", signals);
            }
            if (number(item, "participantCount", &participants)) {
                cJSON_AddNumberToObject(c, "participantCount", participants);
            }
            cJSON_AddItemToArray(cards, c);
        }
    }
    return clip_live_payload("aihot", cards, now);
}
static const char *at(const cJSON *array, int index) {
    const cJSON *v = cJSON_GetArrayItem(array, index);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool parse_number(const char *s, double *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    *out = strtod(s, &end);
    return end && !*end && isfinite(*out);
}
static void stock_series(cJSON *c, const cJSON *data, const char *quote_time) {
    const cJSON *intraday = cJSON_GetObjectItemCaseSensitive(data, "data");
    const char *date = str(intraday, "date");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(intraday, "data");
    int count = cJSON_GetArraySize(rows);
    char normalized[9] = {0};
    int di = 0;
    if (date) {
        for (const char *p = date; *p && di < 8; p++) {
            if (isdigit((unsigned char)*p)) {
                normalized[di++] = *p;
            }
        }
    }
    if (di != 8 || strncmp(normalized, quote_time, 8) || count < 2 || count > 1024) {
        return;
    }
    const char *symbol = str(c, "symbol");
    bool us = symbol && !strncmp(symbol, "us", 2), hk = symbol && !strncmp(symbol, "hk", 2);
    double *prices = malloc((size_t)count * sizeof(*prices));
    int *minutes = malloc((size_t)count * sizeof(*minutes));
    if (!prices || !minutes) {
        free(prices);
        free(minutes);
        return;
    }
    int used = 0;
    for (int i = 0; i < count; i++) {
        const char *row = at(rows, i);
        int hhmm;
        double price;
        if (!row || strlen(row) < 7 || row[4] != ' ' || sscanf(row, "%4d %lf", &hhmm, &price) != 2 ||
            !isfinite(price) || price <= 0) {
            continue;
        }
        int hour = hhmm / 100, minute = hhmm % 100, t = hour * 60 + minute;
        if (hour > 23 || minute > 59) {
            continue;
        }
        if (us) {
            if (t < 570 || t > 960) {
                continue;
            }
            t -= 570;
        } else if (hk) {
            if (!((t >= 570 && t <= 720) || (t >= 780 && t <= 960))) {
                continue;
            }
            t = t <= 720 ? t - 570 : t - 630;
        } else {
            if (!((t >= 570 && t <= 690) || (t >= 780 && t <= 900))) {
                continue;
            }
            t = t <= 690 ? t - 570 : t - 660;
        }
        if (used && t < minutes[used - 1]) {
            continue;
        }
        prices[used] = price;
        minutes[used++] = t;
    }
    if (used >= 2) {
        cJSON *series = cJSON_CreateArray(), *times = cJSON_CreateArray();
        if (series && times) {
            int n = used < 64 ? used : 64;
            for (int i = 0; i < n; i++) {
                int j = i * (used - 1) / (n - 1);
                cJSON_AddItemToArray(series, cJSON_CreateNumber(prices[j]));
                cJSON_AddItemToArray(times, cJSON_CreateNumber(minutes[j]));
            }
            cJSON_AddItemToObject(c, "series", series);
            cJSON_AddItemToObject(c, "seriesMinutes", times);
            cJSON_AddStringToObject(c, "seriesDate", normalized);
        } else {
            cJSON_Delete(series);
            cJSON_Delete(times);
        }
    }
    free(prices);
    free(minutes);
}
cJSON *clip_live_parse_stock_card(const cJSON *r, const char *symbol) {
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "data"), symbol);
    const cJSON *values =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(data, "qt"), symbol);
    const char *name = at(values, 1), *raw_time = at(values, 30);
    char normalized[15] = {0};
    int ti = 0;
    if (raw_time) {
        for (const char *p = raw_time; *p && ti < 14; p++) {
            if (isdigit((unsigned char)*p)) {
                normalized[ti++] = *p;
            }
        }
    }
    const char *time = normalized;
    double price, change, percent;
    if (!name || strlen(time) != 14 || !parse_number(at(values, 3), &price) || price <= 0 ||
        !parse_number(at(values, 31), &change) || !parse_number(at(values, 32), &percent)) {
        return NULL;
    }
    for (int i = 0; i < 14; i++) {
        if (!isdigit((unsigned char)time[i])) {
            return NULL;
        }
    }
    char *title = format("%s · %s", name, symbol),
         *value = format("%.2f\n%+.2f  %+.2f%%", price, change, percent);
    char *detail = format("行情时间 %.2s-%.2s %.2s:%.2s:%.2s\n腾讯财经 · 最新报价，可能延迟", time + 4,
                          time + 6, time + 8, time + 10, time + 12),
         *url = format("https://gu.qq.com/%s", symbol);
    cJSON *c = card(title, value, detail, url);
    free(title);
    free(value);
    free(detail);
    free(url);
    if (c) {
        cJSON_AddStringToObject(c, "name", name);
        cJSON_AddStringToObject(c, "symbol", symbol);
        cJSON_AddStringToObject(c, "quoteTime", time);
        cJSON_AddNumberToObject(c, "price", price);
        cJSON_AddNumberToObject(c, "change", change);
        cJSON_AddNumberToObject(c, "percent", percent);
        bool us = !strncmp(symbol, "us", 2), hk = !strncmp(symbol, "hk", 2);
        const char *ticker = at(values, 2);
        char *history_symbol = format("%s%s", us ? "us" : "", us && ticker ? ticker : symbol);
        if (history_symbol) {
            cJSON_AddStringToObject(c, "historySymbol", history_symbol);
            free(history_symbol);
        }
        cJSON_AddStringToObject(c, "currency", us ? "USD" : hk ? "HKD" : "CNY");
        cJSON_AddStringToObject(c, "market", us ? "美股" : hk ? "港股" : "沪深");
        cJSON_AddNumberToObject(c, "sessionMinutes", us ? 390 : hk ? 330 : 240);
        const char *english = at(values, 46);
        if ((us || hk) && english) {
            cJSON_AddStringToObject(c, "company", english);
        }
        const char *keys[] = {"previousClose", "open",     "high",   "low", "volume", "pe",
                              "marketCap",     "yearHigh", "yearLow"};
        const int indices[] = {4, 5, 33, 34, 6, 39, 45, 48, 49};
        for (int i = 0; i < 9; i++) {
            if (i >= 7 && !us && !hk) {
                continue;
            }
            double v;
            if (parse_number(at(values, indices[i]), &v) && v > 0) {
                cJSON_AddNumberToObject(c, keys[i], v);
            }
        }
        stock_series(c, data, time);
    }
    return c;
}

void clip_live_enrich_hot_card(cJSON *card, const char *html) {
    if (!card || !html) {
        return;
    }
    const char *start = strstr(html, "<script type=\"application/ld+json\">");
    if (!start) {
        return;
    }
    start = strchr(start, '>') + 1;
    const char *end = strstr(start, "</script>");
    if (!end || end - start > 60000) {
        return;
    }
    cJSON *data = cJSON_ParseWithLength(start, (size_t)(end - start)),
          *article = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : data;
    const char *text = str(article, "description");
    if (text && *text) {
        cJSON_AddStringToObject(card, "body", text);
        cJSON_AddBoolToObject(card, "bodyComplete", true);
        cJSON_AddNumberToObject(card, "bodyBytes", (double)strlen(text));
    }
    const char *updated = str(article, "dateModified");
    if (updated) {
        cJSON_AddStringToObject(card, "updatedAt", updated);
    }
    double reports;
    if (number(cJSON_GetObjectItemCaseSensitive(article, "mainEntity"), "numberOfItems", &reports)) {
        cJSON_AddNumberToObject(card, "reportCount", reports);
    }
    cJSON_Delete(data);
}
void clip_live_stock_history(cJSON *card, const cJSON *response, const char *symbol) {
    const cJSON *data =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(response, "data"), symbol);
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(data, "qfqday");
    if (!rows) {
        rows = cJSON_GetObjectItemCaseSensitive(data, "day");
    }
    int n = cJSON_GetArraySize(rows);
    if (n < 2) {
        return;
    }
    cJSON *ranges = cJSON_AddObjectToObject(card, "ranges");
    if (!ranges) {
        return;
    }
    const int windows[] = {5, 22, 66, 250};
    const char *names[] = {"week", "month", "quarter", "year"};
    for (int k = 0; k < 4; k++) {
        int from = n > windows[k] ? n - windows[k] : 0, count = n - from;
        cJSON *range = cJSON_CreateObject(), *prices = cJSON_CreateArray();
        if (!range || !prices) {
            cJSON_Delete(range);
            cJSON_Delete(prices);
            continue;
        }
        int samples = count < 64 ? count : 64;
        bool valid = true;
        for (int i = 0; i < samples; i++) {
            const cJSON *row = cJSON_GetArrayItem(rows, from + i * (count - 1) / (samples - 1));
            double price;
            if (!parse_number(at(row, 2), &price) || price <= 0) {
                valid = false;
                break;
            }
            cJSON_AddItemToArray(prices, cJSON_CreateNumber(price));
        }
        const char *first = at(cJSON_GetArrayItem(rows, from), 0),
                   *last = at(cJSON_GetArrayItem(rows, n - 1), 0);
        /* Reject discontinuous historical fixtures rather than label them as a recent period. */
        if (!first || !last || strlen(first) != 10 || strlen(last) != 10 ||
            (strncmp(first, last, 4) != 0 && atoi(last) - atoi(first) > 1)) {
            valid = false;
        }
        if (valid) {
            cJSON_AddItemToObject(range, "series", prices);
            cJSON_AddStringToObject(range, "start", first);
            cJSON_AddStringToObject(range, "end", last);
            cJSON_AddItemToObject(ranges, names[k], range);
        } else {
            cJSON_Delete(prices);
            cJSON_Delete(range);
        }
    }
}

bool clip_live_stock_symbol(const char *input, char *out, size_t size) {
    if (!input || strlen(input) != 6 || size < 9) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)input[i])) {
            return false;
        }
    }
    const char *market = input[0] == '4' || input[0] == '8' || !strncmp(input, "92", 2) ? "bj"
                         : input[0] == '5' || input[0] == '6' || input[0] == '9'        ? "sh"
                                                                                        : "sz";
    snprintf(out, size, "%s%s", market, input);
    return true;
}

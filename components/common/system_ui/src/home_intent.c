/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0 语音本地指令解析：见 home_intent.h。两条语音线路共用——
 * 底座模式在识别之后、请求对话模型之前调用；背夹·手机模式由手机识别后经 BLE voice_intent 送来。
 */
#include "home_intent.h"

#include <stdio.h>
#include <string.h>

#define HV_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool has(const char *t, const char *k)
{
    return strstr(t, k) != NULL;
}

static bool has_any(const char *t, const char *const *keys, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strstr(t, keys[i])) {
            return true;
        }
    }
    return false;
}

static size_t utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
}

/* 把 buf 里出现的 key 全部换成同样长度的空格（例如“一点”，免得被当成数字 1） */
static void blank_out(char *buf, const char *key)
{
    size_t n = strlen(key);
    for (char *p = strstr(buf, key); p; p = strstr(p + n, key)) {
        memset(p, ' ', n);
    }
}

/* 一个中文数字字的值：零〇一二两三四五六七八九 → 0-9，十 → 10，百 → 100；不是返回 -1 */
static int cn_digit(const char *p, size_t *len)
{
    static const struct {
        const char *s;
        int v;
    } k[] = {
        {"零", 0}, {"〇", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}, {"十", 10}, {"百", 100},
    };
    for (size_t i = 0; i < HV_COUNT(k); i++) {
        size_t n = strlen(k[i].s);
        if (strncmp(p, k[i].s, n) == 0) {
            *len = n;
            return k[i].v;
        }
    }
    return -1;
}

/* 从 p 开始找第一个数：阿拉伯数字，或“百分之四十 / 四十五 / 一百 / 百分之百”；找不到返回 -1 */
static int find_number(const char *p)
{
    static const char k_percent[] = "百分之";
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            int v = 0;
            while (*p >= '0' && *p <= '9' && v <= 1000) {
                v = v * 10 + (*p - '0');
                p++;
            }
            return v;
        }
        if (strncmp(p, k_percent, sizeof(k_percent) - 1) == 0) {
            p += sizeof(k_percent) - 1;
            continue;
        }
        size_t len = 0;
        if (cn_digit(p, &len) >= 0) {
            int total = 0, cur = 0, d;
            while ((d = cn_digit(p, &len)) >= 0) {
                if (d == 10) {
                    total += (cur ? cur : 1) * 10;
                    cur = 0;
                } else if (d == 100) {
                    total += (cur ? cur : 1) * 100;
                    cur = 0;
                } else {
                    cur = d;
                }
                p += len;
            }
            return total + cur;
        }
        p += utf8_len((unsigned char)*p);
    }
    return -1;
}

static int clamp_pct(int v)
{
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

/* 问句：有疑问词又没有“调 / 设 / 改 …”这类动作词，交给模型回答 */
static bool is_question(const char *t)
{
    static const char *const q[] = {"吗", "？", "?", "多少", "几", "什么", "怎么", "是不是"};
    static const char *const act[] = {"调", "设", "改", "放到", "开到", "弄", "变", "换", "切", "降", "提高", "加", "减", "打开", "启动"};
    return has_any(t, q, HV_COUNT(q)) && !has_any(t, act, HV_COUNT(act));
}

/* 音量 / 亮度共用：先看“调到 N”，再看“大一点 / 小一点”，最后看紧跟在关键词后面的数 */
static bool parse_level(const char *t, const char *key_pos, const char *const *up, size_t n_up,
                        const char *const *down, size_t n_down, int step, hv_intent_t *out)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", key_pos ? key_pos : t);
    static const char *const fillers[] = {"一点", "一些", "一下", "一档", "一格"};
    for (size_t i = 0; i < HV_COUNT(fillers); i++) {
        blank_out(buf, fillers[i]);
    }
    int num = find_number(buf);
    bool up_hit = has_any(t, up, n_up);
    bool down_hit = has_any(t, down, n_down);
    if (num >= 0 && num <= 100) {
        out->value = num;
        out->relative = false;
        return true;
    }
    if (up_hit != down_hit) {
        out->value = up_hit ? step : -step;
        out->relative = true;
        return true;
    }
    return false;
}

static void trim_app_name(char *s)
{
    static const char *const tail[] = {"。", "！", "？", "，", ".", "!", "?", ",", " ", "吧", "呗", "呀", "啊",
                                       "一下", "应用程序", "应用", "程序", "app", "App", "APP"};
    static const char *const head[] = {"一下", "下", " ", "，", "我的应用", "那个"};
    bool changed = true;
    while (changed && *s) {
        changed = false;
        for (size_t i = 0; i < HV_COUNT(head); i++) {
            size_t n = strlen(head[i]);
            if (strncmp(s, head[i], n) == 0 && strlen(s) > n) {
                memmove(s, s + n, strlen(s + n) + 1);
                changed = true;
            }
        }
        size_t len = strlen(s);
        for (size_t i = 0; i < HV_COUNT(tail); i++) {
            size_t n = strlen(tail[i]);
            if (len >= n && strcmp(s + len - n, tail[i]) == 0) {
                s[len - n] = '\0';
                changed = true;
                break;
            }
        }
    }
    /* 名称后面还有别的话（“打开天气，看看明天”）：截到第一个标点 */
    static const char *const stops[] = {"，", "。", ",", "然后", "并且", "再"};
    for (size_t i = 0; i < HV_COUNT(stops); i++) {
        char *p = strstr(s, stops[i]);
        if (p && p != s) {
            *p = '\0';
        }
    }
}

bool hv_intent_parse(const char *text, hv_intent_t *out)
{
    if (!text || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const char *t = text;
    if (strlen(t) > 240 || is_question(t)) {
        return false;
    }

    /* 1. 模式：要有“切 / 换 / 改 / 用 …”这类动作，免得“现在是底座模式”被当成命令；
     *    “打开底座 AI 服务”没说模式也没说切换，不算 */
    static const char *const sw[] = {"切", "换", "改", "用", "进入", "打开", "开启", "设成", "设为", "转到", "回到"};
    static const char *const sw_strong[] = {"切", "换", "转到", "回到", "改成", "改为", "设成", "设为"};
    if (has_any(t, sw, HV_COUNT(sw))) {
        static const char *const clip_k[] = {"背夹模式", "手机模式", "背夹·手机", "背夹手机", "到背夹", "成背夹", "回背夹", "蓝牙模式"};
        static const char *const dock_k[] = {"底座", "Wi-Fi模式", "WiFi模式", "wifi模式", "无线模式"};
        bool clip = has_any(t, clip_k, HV_COUNT(clip_k));
        bool dock = has_any(t, dock_k, HV_COUNT(dock_k));
        if (clip != dock && (has(t, "模式") || has_any(t, sw_strong, HV_COUNT(sw_strong)))) {
            out->kind = HV_INTENT_MODE;
            out->dock = dock;
            return true;
        }
    }

    /* 2. 音量 */
    static const char *const vol_k[] = {"音量", "声音", "大声", "小声", "静音", "响一点", "响一些", "响点"};
    if (has_any(t, vol_k, HV_COUNT(vol_k))) {
        if (has(t, "静音") || has(t, "关掉声音") || has(t, "关闭声音")) {
            out->kind = HV_INTENT_VOLUME;
            out->value = 0;
            return true;
        }
        if (has(t, "最大")) {
            out->kind = HV_INTENT_VOLUME;
            out->value = 100;
            return true;
        }
        if (has(t, "最小")) {
            out->kind = HV_INTENT_VOLUME;
            out->value = 10;
            return true;
        }
        static const char *const up[] = {"大一", "大点", "大些", "调大", "增大", "加大", "提高", "调高", "大声", "响"};
        static const char *const down[] = {"小一", "小点", "小些", "调小", "减小", "降低", "调低", "小声", "轻一", "低一", "轻点"};
        const char *k = strstr(t, "音量");
        if (!k) {
            k = strstr(t, "声音");
        }
        if (parse_level(t, k, up, HV_COUNT(up), down, HV_COUNT(down), 10, out)) {
            out->kind = HV_INTENT_VOLUME;
            out->value = out->relative ? out->value : clamp_pct(out->value);
            return true;
        }
        return false;
    }

    /* 3. 亮度 */
    static const char *const bri_k[] = {"亮度", "屏幕亮", "屏幕暗", "调亮", "调暗", "亮一", "暗一", "亮点", "暗点", "最亮", "最暗", "亮些", "暗些"};
    if (has_any(t, bri_k, HV_COUNT(bri_k))) {
        if (has(t, "最亮") || (has(t, "亮度") && has(t, "最大"))) {
            out->kind = HV_INTENT_BRIGHTNESS;
            out->value = 100;
            return true;
        }
        if (has(t, "最暗") || (has(t, "亮度") && has(t, "最小"))) {
            out->kind = HV_INTENT_BRIGHTNESS;
            out->value = 10;
            return true;
        }
        static const char *const up[] = {"亮一", "亮点", "亮些", "调亮", "调高", "提高", "加大", "更亮"};
        static const char *const down[] = {"暗一", "暗点", "暗些", "调暗", "调低", "降低", "减小", "更暗"};
        if (parse_level(t, strstr(t, "亮度"), up, HV_COUNT(up), down, HV_COUNT(down), 15, out)) {
            out->kind = HV_INTENT_BRIGHTNESS;
            if (!out->relative) {
                out->value = out->value < 10 ? 10 : clamp_pct(out->value);
            }
            return true;
        }
        return false;
    }

    /* 4. 打开应用：动词后面的名字交给调用方在应用目录里找，找不到仍交给模型 */
    static const char *const open_k[] = {"打开", "启动", "开启", "运行", "进入", "玩一下", "玩一会", "玩"};
    for (size_t i = 0; i < HV_COUNT(open_k); i++) {
        const char *p = strstr(t, open_k[i]);
        if (!p) {
            continue;
        }
        const char *src = p + strlen(open_k[i]);
        size_t n = strlen(src);
        if (n > sizeof(out->app) - 1) {
            /* 放不下：截在完整的字上，不把多字节字切成两半 */
            n = sizeof(out->app) - 1;
            while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
                n--;
            }
        }
        memcpy(out->app, src, n);
        out->app[n] = '\0';
        trim_app_name(out->app);
        static const char *const not_app[] = {"震动", "蓝牙", "Wi-Fi", "WiFi", "wifi", "声音", "屏幕", "灯", "空调", "电视"};
        if (strlen(out->app) < 3 || has_any(out->app, not_app, HV_COUNT(not_app))) {
            out->app[0] = '\0';
            return false;
        }
        out->kind = HV_INTENT_OPEN;
        return true;
    }
    return false;
}

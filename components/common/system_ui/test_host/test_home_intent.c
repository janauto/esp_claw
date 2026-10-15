/*
 * home_intent 宿主测试：cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
 *   -I../src ../src/home_intent.c test_home_intent.c -o /tmp/t && /tmp/t
 */
#include <stdio.h>
#include <string.h>

#include "home_intent.h"

static int g_fail, g_n;

static void expect(const char *text, hv_intent_kind_t kind, int value, int relative, int dock, const char *app)
{
    hv_intent_t it;
    bool ok = hv_intent_parse(text, &it);
    g_n++;
    bool good = kind == HV_INTENT_NONE ? !ok
                                       : ok && it.kind == kind && (value == -999 || it.value == value) &&
                                             (relative < 0 || it.relative == (relative != 0)) &&
                                             (dock < 0 || it.dock == (dock != 0)) && (!app || strcmp(it.app, app) == 0);
    if (!good) {
        g_fail++;
        printf("FAIL %s -> ok=%d kind=%d value=%d rel=%d dock=%d app=%s\n", text, ok, it.kind, it.value, it.relative,
               it.dock, it.app);
    }
}

int main(void)
{
    /* 音量 */
    expect("背夹，把音量调到百分之四十。", HV_INTENT_VOLUME, 40, 0, -1, NULL);
    expect("背夹，把音量调到40%。", HV_INTENT_VOLUME, 40, 0, -1, NULL);
    expect("音量调到一百", HV_INTENT_VOLUME, 100, 0, -1, NULL);
    expect("音量调到百分之百", HV_INTENT_VOLUME, 100, 0, -1, NULL);
    expect("把声音开到五十五", HV_INTENT_VOLUME, 55, 0, -1, NULL);
    expect("音量二十", HV_INTENT_VOLUME, 20, 0, -1, NULL);
    expect("声音大一点", HV_INTENT_VOLUME, 10, 1, -1, NULL);
    expect("背夹，调大音量", HV_INTENT_VOLUME, 10, 1, -1, NULL);
    expect("大声一点", HV_INTENT_VOLUME, 10, 1, -1, NULL);
    expect("音量小一点吧", HV_INTENT_VOLUME, -10, 1, -1, NULL);
    expect("静音", HV_INTENT_VOLUME, 0, 0, -1, NULL);
    expect("音量开到最大", HV_INTENT_VOLUME, 100, 0, -1, NULL);
    expect("现在音量是多少？", HV_INTENT_NONE, 0, -1, -1, NULL);
    expect("帮我调一下音量", HV_INTENT_NONE, 0, -1, -1, NULL);
    expect("背夹，把音量调到百分之。", HV_INTENT_NONE, 0, -1, -1, NULL);   /* 识别不全：交给模型追问 */
    /* 亮度 */
    expect("亮度调到六十", HV_INTENT_BRIGHTNESS, 60, 0, -1, NULL);
    expect("屏幕亮一点", HV_INTENT_BRIGHTNESS, 15, 1, -1, NULL);
    expect("把屏幕调暗一些", HV_INTENT_BRIGHTNESS, -15, 1, -1, NULL);
    expect("亮度调到5", HV_INTENT_BRIGHTNESS, 10, 0, -1, NULL);
    expect("屏幕最亮", HV_INTENT_BRIGHTNESS, 100, 0, -1, NULL);
    /* 模式 */
    expect("背夹，切换到底座模式", HV_INTENT_MODE, -999, -1, 1, NULL);
    expect("切到背夹模式", HV_INTENT_MODE, -999, -1, 0, NULL);
    expect("换成手机模式吧", HV_INTENT_MODE, -999, -1, 0, NULL);
    expect("现在是底座模式吗", HV_INTENT_NONE, 0, -1, -1, NULL);
    /* 不是模式切换；作为“打开”交给调用方，目录里没有这个应用时仍交给模型 */
    expect("打开底座AI服务", HV_INTENT_OPEN, -999, -1, -1, "底座AI服务");
    /* 打开应用 */
    expect("背夹，请打开天气应用。", HV_INTENT_OPEN, -999, -1, -1, "天气");
    expect("打开贪吃蛇", HV_INTENT_OPEN, -999, -1, -1, "贪吃蛇");
    expect("帮我启动一下会议录音吧", HV_INTENT_OPEN, -999, -1, -1, "会议录音");
    expect("我想玩2048", HV_INTENT_OPEN, -999, -1, -1, "2048");
    expect("打开我的花园，然后浇浇水", HV_INTENT_OPEN, -999, -1, -1, "我的花园");
    expect("打开蓝牙", HV_INTENT_NONE, 0, -1, -1, NULL);
    expect("今天天气怎么样？", HV_INTENT_NONE, 0, -1, -1, NULL);
    expect("给我讲个笑话", HV_INTENT_NONE, 0, -1, -1, NULL);
    expect("", HV_INTENT_NONE, 0, -1, -1, NULL);
    {
        char longtext[400];
        memset(longtext, 0, sizeof(longtext));
        strcpy(longtext, "打开");
        for (int i = 0; i < 30; i++) {
            strcat(longtext, "贪吃");
        }
        hv_intent_t it;
        bool ok = hv_intent_parse(longtext, &it);
        g_n++;
        /* 截断后仍是完整 UTF-8 */
        size_t n = strlen(it.app);
        if (!ok || n == 0 || n % 3 != 0) {
            g_fail++;
            printf("FAIL long name ok=%d n=%zu\n", ok, n);
        }
    }
    printf("%s: %d cases, %d failures\n", g_fail ? "FAILED" : "PASSED", g_n, g_fail);
    return g_fail ? 1 : 0;
}

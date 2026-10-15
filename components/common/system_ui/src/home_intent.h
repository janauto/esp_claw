/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0 语音本地指令解析（纯文本处理，不依赖 LVGL / ESP-IDF，宿主可测）。
 *
 * 能确定的设备操作（调音量、调亮度、切换模式、打开应用）由背夹自己执行，不交给对话模型：
 * 实测 deepseek-flash 偶尔只回一句“音量已经调到 40% 了”却不调用工具，用户以为调好了。
 * 只认清楚的命令；问句、没说数值也没说“大一点/小一点”的句子仍交给模型。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HV_INTENT_NONE = 0,
    HV_INTENT_VOLUME,
    HV_INTENT_BRIGHTNESS,
    HV_INTENT_MODE,
    HV_INTENT_OPEN,
} hv_intent_kind_t;

typedef struct {
    hv_intent_kind_t kind;
    int value;          /* 音量/亮度：目标百分比；relative 时为增量（可为负） */
    bool relative;
    bool dock;          /* 模式：true = 底座·Wi-Fi，false = 背夹·手机 */
    char app[64];       /* 打开应用：动词后面的名称，已去掉“应用 / 一下 / 吧”和标点 */
} hv_intent_t;

/* 解析一句识别文字；认出命令返回 true 并填 out。 */
bool hv_intent_parse(const char *text, hv_intent_t *out);

#ifdef __cplusplus
}
#endif

/*
 * 地址拼接、配置校验、请求 JSON 构造与响应解析。
 *
 * 请求格式照搬已验证的实现：
 * - ASR：语音中继 relay/voice_relay.py 的 transcribe()（百炼兼容模式 + input_audio data URI + 热词 system）；
 * - 对话：手机端 chat_agent.dart（OpenAI 兼容 + tools，DeepSeek 关闭思考）。
 * 纯 C + cJSON；只有内存分配与错误名在 ESP_PLATFORM 下走 IDF 接口。
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "vc_internal.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* ---------------- 内存与日志 ---------------- */

void *vc_big_malloc(size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
#else
    return malloc(size);
#endif
}

void *vc_big_realloc(void *ptr, size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : realloc(ptr, size);
#else
    return realloc(ptr, size);
#endif
}

void vc_big_free(void *ptr)
{
    free(ptr);
}

char *vc_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

void vc_secure_free(char *s)
{
    if (!s) {
        return;
    }
    volatile char *v = s;
    while (*v) {
        *v++ = 0;
    }
    free(s);
}

#ifndef ESP_PLATFORM
void vc_host_log(char level, const char *fmt, ...)
{
    if (!getenv("VC_LOG")) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c voice_cloud: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#endif

/* ---------------- 地址 ---------------- */

static bool ends_with(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    return len >= n && strncmp(s + len - n, suffix, n) == 0;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

int vc_base_normalize(const char *in, char *out, size_t out_size, bool strip_suffix)
{
    if (!in || !out || out_size == 0) {
        return -1;
    }
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    size_t len = strlen(in);
    while (len && (in[len - 1] == ' ' || in[len - 1] == '\t')) {
        len--;
    }
    if (len < 9 || len > VC_BASE_MAX || strncasecmp(in, "https://", 8) != 0) {
        return -1; /* 只接受 HTTPS，避免密钥明文上网 */
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c <= 0x20 || c >= 0x7F || c == '@' || c == '?' || c == '#' || c == '\\' || c == '"' || c == '<' ||
            c == '>' || c == '`' || c == '{' || c == '}' || c == '|' || c == '^') {
            return -1; /* 不允许用户名、查询串、片段等可能夹带密钥的成分 */
        }
    }
    /* 主机部分：字母数字 . - 与可选 :端口 */
    size_t h = 8;
    while (h < len && in[h] != '/') {
        char c = in[h];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':')) {
            return -1;
        }
        h++;
    }
    if (h == 8) {
        return -1;
    }
    while (len > 8 && in[len - 1] == '/') {
        len--;
    }
    if (strip_suffix) {
        if (ends_with(in, len, "/chat/completions")) {
            len -= strlen("/chat/completions");
        }
        if (ends_with(in, len, "/compatible-mode/v1")) {
            len -= strlen("/compatible-mode/v1");
        }
        while (len > 8 && in[len - 1] == '/') {
            len--;
        }
    }
    if (len + 1 > out_size) {
        return -1;
    }
    memcpy(out, "https://", 8);
    memcpy(out + 8, in + 8, len - 8);
    out[len] = '\0';
    return 0;
}

int vc_url_host(const char *url, char *host, size_t host_size)
{
    if (!url || !host || host_size == 0 || strncasecmp(url, "https://", 8) != 0) {
        return -1;
    }
    const char *s = url + 8;
    size_t n = strcspn(s, "/");
    if (n == 0 || n + 1 > host_size) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        host[i] = (char)tolower((unsigned char)s[i]);
    }
    host[n] = '\0';
    return 0;
}

int vc_asr_url(const char *asr_base, char *out, size_t out_size)
{
    char base[VC_BASE_MAX + 1];
    const char *src = (asr_base && asr_base[0]) ? asr_base : VC_DEFAULT_ASR_BASE;
    if (vc_base_normalize(src, base, sizeof(base), true) != 0) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/compatible-mode/v1/chat/completions", base);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

int vc_chat_url(const char *chat_base, const char *asr_base, const char *model,
                char *out, size_t out_size, vc_flavor_t *flavor)
{
    char base[VC_BASE_MAX + 1];
    char host[VC_HOST_MAX];
    char asr_host[VC_HOST_MAX] = "";
    char asr_norm[VC_BASE_MAX + 1];
    const char *src = (chat_base && chat_base[0]) ? chat_base : VC_DEFAULT_CHAT_BASE;
    if (vc_base_normalize(src, base, sizeof(base), false) != 0 || vc_url_host(base, host, sizeof(host)) != 0) {
        return -1;
    }
    size_t len = strlen(base);
    if (ends_with(base, len, "/chat/completions")) {
        len -= strlen("/chat/completions");
        base[len] = '\0';
    }
    const char *asr_src = (asr_base && asr_base[0]) ? asr_base : VC_DEFAULT_ASR_BASE;
    if (vc_base_normalize(asr_src, asr_norm, sizeof(asr_norm), true) == 0) {
        (void)vc_url_host(asr_norm, asr_host, sizeof(asr_host));
    }
    vc_flavor_t fl;
    int n;
    if (strstr(base, "/compatible-mode/")) {
        fl = VC_FLAVOR_DASHSCOPE;
        n = snprintf(out, out_size, "%s/chat/completions", base);
    } else if (contains_ci(host, "dashscope") || (asr_host[0] && strcmp(host, asr_host) == 0)) {
        /* 百炼域名（含业务空间专属域名，与 ASR 同域）走兼容模式路径 */
        fl = VC_FLAVOR_DASHSCOPE;
        n = snprintf(out, out_size, "%s/compatible-mode/v1/chat/completions", base);
    } else {
        fl = (contains_ci(host, "deepseek") || (model && strncasecmp(model, "deepseek", 8) == 0))
                 ? VC_FLAVOR_DEEPSEEK
                 : VC_FLAVOR_OPENAI;
        n = snprintf(out, out_size, "%s/chat/completions", base);
    }
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    if (flavor) {
        *flavor = fl;
    }
    return 0;
}

bool vc_key_valid(const char *key)
{
    if (!key) {
        return false;
    }
    size_t n = strlen(key);
    if (n == 0 || n > VC_KEY_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c <= 0x20 || c >= 0x7F) {
            return false;
        }
    }
    return true;
}

bool vc_model_valid(const char *model)
{
    if (!model) {
        return false;
    }
    size_t n = strlen(model);
    if (n == 0 || n > VC_MODEL_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)model[i];
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

/* ---------------- ASR 请求 ---------------- */

#define VC_AUDIO_MARK "@@VC_AUDIO_DATA@@"
static const char k_data_uri[] = "data:audio/ogg;base64,";

typedef struct {
    vc_b64_t *b64;
} vc_b64_sink_t;

static int b64_sink(void *ctx, const uint8_t *data, size_t len)
{
    vc_b64_sink_t *s = ctx;
    vc_b64_update(s->b64, data, len);
    return s->b64->overflow ? -1 : 0;
}

char *vc_asr_build_body(const uint8_t *frames, size_t frames_len, const char *model,
                        const char *context, size_t *out_len, size_t *ogg_len)
{
    vc_ogg_opts_t o;
    vc_ogg_opts_default(&o);
    size_t ogg = vc_ogg_size(frames, frames_len, &o);
    if (!ogg || !model || !out_len) {
        return NULL;
    }

    /* 先用 cJSON 生成骨架（负责转义），音频位置放占位符，再拼成最终正文；大块 base64 不经过 cJSON。 */
    cJSON *root = cJSON_CreateObject();
    cJSON *msgs = (root && cJSON_AddStringToObject(root, "model", model)) ? cJSON_AddArrayToObject(root, "messages")
                                                                          : NULL;
    if (!msgs) {
        cJSON_Delete(root);
        return NULL;
    }
    bool ok = true;
    if (context && context[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON *content = sys ? cJSON_AddArrayToObject(sys, "content") : NULL;
        cJSON *part = cJSON_CreateObject();
        ok = sys && content && part && cJSON_AddStringToObject(sys, "role", "system") &&
             cJSON_AddStringToObject(part, "type", "text") && cJSON_AddStringToObject(part, "text", context);
        if (part && !(content && cJSON_AddItemToArray(content, part))) {
            cJSON_Delete(part);
            ok = false;
        }
        if (sys && !cJSON_AddItemToArray(msgs, sys)) {
            cJSON_Delete(sys);
            ok = false;
        }
    }
    cJSON *user = cJSON_CreateObject();
    cJSON *ucontent = user ? cJSON_AddArrayToObject(user, "content") : NULL;
    cJSON *audio = cJSON_CreateObject();
    cJSON *input = audio ? cJSON_AddObjectToObject(audio, "input_audio") : NULL;
    ok = ok && user && ucontent && audio && input && cJSON_AddStringToObject(user, "role", "user") &&
         cJSON_AddStringToObject(audio, "type", "input_audio") && cJSON_AddStringToObject(input, "data", VC_AUDIO_MARK);
    if (audio && !(ucontent && cJSON_AddItemToArray(ucontent, audio))) {
        cJSON_Delete(audio);
        ok = false;
    }
    if (user && !cJSON_AddItemToArray(msgs, user)) {
        cJSON_Delete(user);
        ok = false;
    }
    cJSON *opts = cJSON_AddObjectToObject(root, "asr_options");
    ok = ok && cJSON_AddFalseToObject(root, "stream") && opts && cJSON_AddTrueToObject(opts, "enable_itn") &&
         cJSON_AddStringToObject(opts, "language", "zh");
    char *skel = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!skel) {
        return NULL;
    }
    char *mark = strstr(skel, "\"" VC_AUDIO_MARK "\"");
    if (!mark) {
        cJSON_free(skel);
        return NULL;
    }
    size_t prefix_len = (size_t)(mark - skel) + 1; /* 含左引号 */
    const char *suffix = mark + 1 + strlen(VC_AUDIO_MARK); /* 从右引号开始 */
    size_t suffix_len = strlen(suffix);
    size_t b64_len = vc_b64_len(ogg);
    size_t uri_len = sizeof(k_data_uri) - 1;
    size_t total = prefix_len + uri_len + b64_len + suffix_len;
    char *body = vc_big_malloc(total + 1);
    if (!body) {
        cJSON_free(skel);
        return NULL;
    }
    memcpy(body, skel, prefix_len);
    memcpy(body + prefix_len, k_data_uri, uri_len);
    vc_b64_t b64;
    vc_b64_init(&b64, body + prefix_len + uri_len, b64_len);
    vc_b64_sink_t sink = {&b64};
    int r = vc_ogg_mux(frames, frames_len, &o, b64_sink, &sink);
    size_t written = vc_b64_final(&b64);
    if (r != 0 || written != b64_len) {
        cJSON_free(skel);
        vc_big_free(body);
        return NULL;
    }
    memcpy(body + prefix_len + uri_len + b64_len, suffix, suffix_len);
    body[total] = '\0';
    cJSON_free(skel);
    *out_len = total;
    if (ogg_len) {
        *ogg_len = ogg;
    }
    return body;
}

/* 去首尾 ASCII 空白，返回新字符串 */
static char *dup_trim(const char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

/* content 可能是字符串，也可能是 [{"text":…}] 分段数组（少数版本） */
static char *content_text(const cJSON *content, bool *bad)
{
    if (cJSON_IsString(content)) {
        return dup_trim(content->valuestring);
    }
    if (!content || cJSON_IsNull(content)) {
        return dup_trim("");
    }
    if (!cJSON_IsArray(content)) {
        *bad = true;
        return NULL;
    }
    size_t total = 0;
    const cJSON *part;
    cJSON_ArrayForEach(part, content) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (cJSON_IsString(t)) {
            total += strlen(t->valuestring);
        }
    }
    char *joined = malloc(total + 1);
    if (!joined) {
        return NULL;
    }
    size_t pos = 0;
    cJSON_ArrayForEach(part, content) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (cJSON_IsString(t)) {
            size_t n = strlen(t->valuestring);
            memcpy(joined + pos, t->valuestring, n);
            pos += n;
        }
    }
    joined[pos] = '\0';
    char *trimmed = dup_trim(joined);
    free(joined);
    return trimmed;
}

static const cJSON *first_message(const cJSON *root)
{
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) {
        return NULL;
    }
    const cJSON *choice = cJSON_GetArrayItem(choices, 0);
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(choice, "message");
    return cJSON_IsObject(msg) ? msg : NULL;
}

int vc_asr_parse(const char *body, size_t len, char **text)
{
    *text = NULL;
    if (!body || !len) {
        return -1;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    const cJSON *msg = first_message(root);
    bool bad = false;
    char *t = msg ? content_text(cJSON_GetObjectItemCaseSensitive(msg, "content"), &bad) : NULL;
    cJSON_Delete(root);
    if (!t) {
        return -1;
    }
    *text = t;
    return 0;
}

/* ---------------- 对话请求 ---------------- */

static const char k_system_prompt[] =
    "你是“品胜磁吸背夹”上的语音助手。用户对着背夹说话，你的回答会显示在背夹的小屏幕上。\n"
    "要求：用中文口语回答，简短直接，不超过 80 个字；不要使用 Markdown、列表、表情符号或特殊符号。\n"
    "用户明确要求操作设备（调亮度、调音量、切换模式、打开应用）时，必须调用对应的工具，"
    "不要只用文字回答，也不要假装已经完成；用户没有提出时不要擅自操作设备。工具失败时如实告诉用户。\n"
    "没听清或意思不明确时，请用户再说一遍。";

char *vc_system_prompt(const char *now_text)
{
    size_t n = sizeof(k_system_prompt) + (now_text ? strlen(now_text) + 32 : 0);
    char *s = malloc(n);
    if (!s) {
        return NULL;
    }
    if (now_text && now_text[0]) {
        snprintf(s, n, "%s\n当前时间：%s。", k_system_prompt, now_text);
    } else {
        memcpy(s, k_system_prompt, sizeof(k_system_prompt));
    }
    return s;
}

static bool add_message(cJSON *arr, const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    if (!m || !cJSON_AddStringToObject(m, "role", role) || !cJSON_AddStringToObject(m, "content", content ? content : "") ||
        !cJSON_AddItemToArray(arr, m)) {
        cJSON_Delete(m);
        return false;
    }
    return true;
}

cJSON *vc_chat_messages(const char *system, const vc_turn_t *hist, size_t hist_n, const char *user)
{
    cJSON *arr = cJSON_CreateArray();
    bool ok = arr && add_message(arr, "system", system);
    for (size_t i = 0; ok && i < hist_n; i++) {
        ok = add_message(arr, "user", hist[i].user) && add_message(arr, "assistant", hist[i].assistant);
    }
    ok = ok && add_message(arr, "user", user);
    if (!ok) {
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
}

char *vc_chat_build_body(const char *model, vc_flavor_t flavor, const cJSON *messages,
                         const cJSON *tools, const char *tool_choice, size_t *out_len)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root && model && messages && cJSON_AddStringToObject(root, "model", model) &&
              cJSON_AddItemReferenceToObject(root, "messages", (cJSON *)messages) &&
              cJSON_AddFalseToObject(root, "stream") && cJSON_AddNumberToObject(root, "max_tokens", VC_CHAT_MAX_TOKENS) &&
              cJSON_AddNumberToObject(root, "temperature", 0.5);
    if (ok && tools && tool_choice) {
        ok = cJSON_AddItemReferenceToObject(root, "tools", (cJSON *)tools) &&
             cJSON_AddStringToObject(root, "tool_choice", tool_choice);
    }
    if (ok && flavor == VC_FLAVOR_DEEPSEEK) {
        /* DeepSeek flash 默认先“思考”，语音对答要低延迟，关掉 */
        cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
        ok = thinking && cJSON_AddStringToObject(thinking, "type", "disabled");
    } else if (ok && flavor == VC_FLAVOR_DASHSCOPE) {
        ok = cJSON_AddFalseToObject(root, "enable_thinking") != NULL;
    }
    char *out = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root); /* 引用项不会释放 messages/tools 本体 */
    if (out && out_len) {
        *out_len = strlen(out);
    }
    return out;
}

cJSON *vc_chat_parse_message(const char *body, size_t len)
{
    if (!body || !len) {
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = cJSON_IsObject(choice) ? cJSON_DetachItemFromObjectCaseSensitive(choice, "message") : NULL;
    cJSON_Delete(root);
    if (msg && !cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        msg = NULL;
    }
    return msg;
}

char *vc_message_text(const cJSON *message)
{
    bool bad = false;
    char *t = content_text(cJSON_GetObjectItemCaseSensitive(message, "content"), &bad);
    if (!t && bad) {
        t = vc_strdup("");
    }
    return t;
}

/* ---------------- 工具定义 ---------------- */

static bool tool_name_valid(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '-')) {
            return false;
        }
    }
    return true;
}

static const char *tool_name(const cJSON *tool)
{
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
    return cJSON_IsString(name) ? name->valuestring : NULL;
}

char *vc_tools_normalize(const char *tools_json)
{
    if (!tools_json || strlen(tools_json) > VC_TOOLS_JSON_MAX) {
        return NULL;
    }
    cJSON *arr = cJSON_Parse(tools_json);
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    bool ok = n > 0 && n <= VC_TOOLS_MAX;
    const cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        if (!ok) {
            break;
        }
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(tool, "type");
        const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool, "function");
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(fn, "parameters");
        ok = cJSON_IsObject(tool) && cJSON_IsString(type) && strcmp(type->valuestring, "function") == 0 &&
             cJSON_IsObject(fn) && tool_name_valid(tool_name(tool)) && (!params || cJSON_IsObject(params));
        /* 不允许重名 */
        for (const cJSON *prev = arr->child; ok && prev && prev != tool; prev = prev->next) {
            ok = strcmp(tool_name(prev), tool_name(tool)) != 0;
        }
    }
    char *out = ok ? cJSON_PrintUnformatted(arr) : NULL;
    cJSON_Delete(arr);
    if (!out) {
        return NULL;
    }
    char *copy = vc_strdup(out); /* 统一用 free 释放 */
    cJSON_free(out);
    return copy;
}

bool vc_tools_has(const cJSON *tools, const char *name)
{
    const cJSON *tool;
    if (!name) {
        return false;
    }
    cJSON_ArrayForEach(tool, tools) {
        const char *n = tool_name(tool);
        if (n && strcmp(n, name) == 0) {
            return true;
        }
    }
    return false;
}

static const char k_default_tools[] =
    "[{\"type\":\"function\",\"function\":{\"name\":\"open_app\","
    "\"description\":\"打开背夹上的应用。只在用户明确要求打开某个应用时调用，"
    "例如天气、会议纪要、红外遥控、我的花园。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\","
    "\"description\":\"应用名称，按用户的说法填写\"}},\"required\":[\"name\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_brightness\",\"description\":\"设置屏幕亮度。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\",\"minimum\":5,"
    "\"maximum\":100,\"description\":\"亮度百分比，5 到 100\"}},\"required\":[\"percent\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_volume\",\"description\":\"设置扬声器音量。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\",\"minimum\":0,"
    "\"maximum\":100,\"description\":\"音量百分比，0 到 100\"}},\"required\":[\"percent\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"set_mode\","
    "\"description\":\"切换工作模式：clip 是背夹·手机模式（语音经蓝牙交给手机处理），"
    "dock 是底座·Wi-Fi 模式（背夹自己连云端）。\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\","
    "\"enum\":[\"clip\",\"dock\"]}},\"required\":[\"mode\"]}}}]";

const char *voice_cloud_default_tools_json(void)
{
    return k_default_tools;
}

/* ---------------- 错误 ---------------- */

const char *vc_err_http(vc_stage_t stage, int status, char *detail, size_t detail_size)
{
    bool asr = stage == VC_STAGE_ASR;
    if (detail && detail_size) {
        snprintf(detail, detail_size, "%s_http_%d", asr ? "asr" : "chat", status);
    }
    if (status == 401 || status == 403) {
        return asr ? "语音识别服务鉴权失败，请检查密钥" : "对话服务鉴权失败，请检查密钥";
    }
    if (status == 402) {
        return "对话服务账户余额不足";
    }
    if (status == 404) {
        return "服务地址或模型名不对，请检查配置";
    }
    if (status == 429) {
        return "服务太忙了，请稍后再试";
    }
    if (status >= 500) {
        return "服务暂时不可用，请稍后再试";
    }
    if (status == 400) {
        return asr ? "语音识别请求被拒绝，请再说一次" : "对话请求被拒绝，请换个说法";
    }
    return "服务出错，请稍后再试";
}

const char *vc_err_name(esp_err_t err)
{
#ifdef ESP_PLATFORM
    return esp_err_to_name(err);
#else
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "ESP_ERR_UNKNOWN";
    }
#endif
}

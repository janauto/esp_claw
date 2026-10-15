/*
 * voice_cloud 设备胶水：NVS 配置、收包缓存、会话与工作任务、事件回调。
 *
 * 锁的划分：
 * - s_store_mux（自旋临界区）：只保护收包缓存的指针与计数，push_opus 在里面做一次 ≤1275 字节的拷贝，
 *   不分配内存、不等待，可在音频/蓝牙相关任务里调用；
 * - s_vc.lock（互斥量）：保护配置、工具、历史与会话状态；任何用户回调都不在持锁时调用，避免与 UI 锁互相等待。
 * 会话：finish/ask_text 复制一份配置快照交给工作任务，任务结束时释放全部内存（密钥副本先清零）。
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "vc_internal.h"
#include "voice_cloud.h"

/* 工作任务参数，可在组件外用 target_compile_definitions 覆盖 */
#ifndef VOICE_CLOUD_TASK_STACK
#define VOICE_CLOUD_TASK_STACK 10240 /* TLS 握手 + cJSON + 回调，余量见 VALIDATION.md 的待实测项 */
#endif
#ifndef VOICE_CLOUD_TASK_PRIO
#define VOICE_CLOUD_TASK_PRIO 5
#endif
#ifndef VOICE_CLOUD_TASK_STACK_PSRAM
/* 默认把栈放内部 RAM：工具回调可能写 NVS（例如 set_mode 持久化），Flash 写入期间若关闭 Cache，
 * PSRAM 上的栈不可访问。内部 RAM 不足时自动退回 PSRAM。设为 1 则优先 PSRAM。 */
#define VOICE_CLOUD_TASK_STACK_PSRAM 0
#endif

static const char *TAG = "voice_cloud";
#define VC_NVS_NS "vcloud"
#define VC_HIST_USER_MAX 600 /* 写进历史的用户话上限（字节） */

typedef struct {
    char asr_base[VC_BASE_MAX + 1];
    char asr_key[VC_KEY_MAX + 1];
    char asr_model[VC_MODEL_MAX + 1];
    char chat_base[VC_BASE_MAX + 1];
    char chat_key[VC_KEY_MAX + 1];
    char chat_model[VC_MODEL_MAX + 1];
} vc_config_t;

typedef struct vc_session {
    atomic_bool cancelled;
    voice_cloud_event_cb_t cb;
    void *cb_ctx;
    vc_job_t job;
    char asr_url[VC_URL_MAX];
    char chat_url[VC_URL_MAX];
    char asr_model[VC_MODEL_MAX + 1];
    char chat_model[VC_MODEL_MAX + 1];
    char *asr_key;  /* 密钥副本，释放前清零 */
    char *chat_key;
    char *asr_context;
    uint8_t *frames;
    size_t frames_len;
    size_t packets;
    char *text;
    char *tools_json;
    vc_turn_t hist[VC_HISTORY_TURNS];
    size_t hist_n;
    char now_text[64];
    bool stack_psram;
    int64_t started_ms;
} vc_session_t;

static struct {
    bool inited;
    SemaphoreHandle_t lock;
    vc_config_t *cfg;
    char *tools_json;
    voice_cloud_tool_cb_t tool_cb;
    void *tool_ctx;
    vc_turn_t hist[VC_HISTORY_TURNS];
    size_t hist_n;
    vc_session_t *current; /* 最近一次启动的会话（可能已取消但还在收尾） */
    int live;              /* 存活的工作任务数（最多 2：一个收尾中的已取消会话 + 一个新会话） */
    char *asr_context;     /* 热词覆盖（测试/核验用），NULL = 默认 */
} s_vc;

static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_store_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t *buf;
    size_t cap;
    size_t used;
    size_t packets;
    size_t dropped;
    size_t last_packets;
    bool receiving;
} s_store;

static const char *const k_fields[6] = {"asr_base", "asr_key", "asr_model", "chat_base", "chat_key", "chat_model"};

/* ---------------- 小工具 ---------------- */

static void lock(void)
{
    xSemaphoreTake(s_vc.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_vc.lock);
}

static int64_t vc_now_ms(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time() / 1000;
}

static void *vc_calloc_big(size_t size)
{
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, size);
}

static void wipe_free(void *p, size_t size)
{
    if (p) {
        volatile uint8_t *v = p;
        for (size_t i = 0; i < size; i++) {
            v[i] = 0;
        }
        free(p);
    }
}

static char *field(vc_config_t *c, int i)
{
    switch (i) {
    case 0: return c->asr_base;
    case 1: return c->asr_key;
    case 2: return c->asr_model;
    case 3: return c->chat_base;
    case 4: return c->chat_key;
    default: return c->chat_model;
    }
}

static size_t field_size(int i)
{
    return (i == 0 || i == 3) ? VC_BASE_MAX + 1 : (i == 1 || i == 4) ? VC_KEY_MAX + 1 : VC_MODEL_MAX + 1;
}

/* 校验一个字段并写入 out；"" 表示清除。返回 false 表示非法。 */
static bool field_accept(int i, const char *value, char *out, size_t size)
{
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        value++;
    }
    size_t n = strlen(value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == '\r' || value[n - 1] == '\n')) {
        n--;
    }
    if (n == 0) {
        out[0] = '\0';
        return true;
    }
    if (n >= size) {
        return false;
    }
    char tmp[VC_KEY_MAX + 1];
    memcpy(tmp, value, n);
    tmp[n] = '\0';
    bool ok;
    if (i == 0 || i == 3) {
        ok = vc_base_normalize(tmp, out, size, i == 0) == 0; /* ASR 根地址去掉多余路径 */
    } else if (i == 1 || i == 4) {
        ok = vc_key_valid(tmp);
        if (ok) {
            memcpy(out, tmp, n + 1);
        }
    } else {
        ok = vc_model_valid(tmp);
        if (ok) {
            memcpy(out, tmp, n + 1);
        }
    }
    memset(tmp, 0, sizeof(tmp));
    return ok;
}

static const char *eff_asr_model(const vc_config_t *c)
{
    return c->asr_model[0] ? c->asr_model : VC_DEFAULT_ASR_MODEL;
}

static const char *eff_chat_model(const vc_config_t *c)
{
    return c->chat_model[0] ? c->chat_model : VC_DEFAULT_CHAT_MODEL;
}

static bool asr_ready_locked(void)
{
    char url[VC_URL_MAX];
    return s_vc.cfg && s_vc.cfg->asr_key[0] && vc_asr_url(s_vc.cfg->asr_base, url, sizeof(url)) == 0;
}

static bool chat_ready_locked(void)
{
    char url[VC_URL_MAX];
    return s_vc.cfg && s_vc.cfg->chat_key[0] &&
           vc_chat_url(s_vc.cfg->chat_base, s_vc.cfg->asr_base, eff_chat_model(s_vc.cfg), url, sizeof(url), NULL) == 0;
}

static bool busy_locked(void)
{
    return s_vc.current && !atomic_load(&s_vc.current->cancelled);
}

/* ---------------- NVS ---------------- */

static esp_err_t cfg_load(vc_config_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t h;
    esp_err_t err = nvs_open(VC_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* 还没配置过 */
    }
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 6; i++) {
        char *dst = field(c, i);
        size_t len = field_size(i);
        if (nvs_get_str(h, k_fields[i], dst, &len) != ESP_OK) {
            dst[0] = '\0';
            continue;
        }
        /* 存储里的值也过一遍校验，坏值当作未配置 */
        char tmp[VC_KEY_MAX + 1];
        if (!field_accept(i, dst, tmp, field_size(i))) {
            dst[0] = '\0';
        } else {
            memcpy(dst, tmp, strlen(tmp) + 1);
        }
        memset(tmp, 0, sizeof(tmp));
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t voice_cloud_init(void)
{
    portENTER_CRITICAL(&s_init_mux);
    bool need_lock = s_vc.lock == NULL;
    portEXIT_CRITICAL(&s_init_mux);
    if (need_lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (!m) {
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_init_mux);
        if (s_vc.lock == NULL) {
            s_vc.lock = m;
            m = NULL;
        }
        portEXIT_CRITICAL(&s_init_mux);
        if (m) {
            vSemaphoreDelete(m);
        }
    }
    vc_config_t *loaded = vc_calloc_big(sizeof(*loaded));
    if (!loaded) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = cfg_load(loaded);
    if (err != ESP_OK) {
        wipe_free(loaded, sizeof(*loaded));
        ESP_LOGW(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }
    lock();
    vc_config_t *old = s_vc.cfg;
    s_vc.cfg = loaded;
    s_vc.inited = true;
    bool asr = asr_ready_locked(), chat = chat_ready_locked();
    unlock();
    wipe_free(old, sizeof(*old));
    ESP_LOGI(TAG, "init: asr_ready=%d chat_ready=%d", asr, chat);
    return ESP_OK;
}

bool voice_cloud_asr_ready(void)
{
    if (!s_vc.inited) {
        return false;
    }
    lock();
    bool r = asr_ready_locked();
    unlock();
    return r;
}

bool voice_cloud_chat_ready(void)
{
    if (!s_vc.inited) {
        return false;
    }
    lock();
    bool r = chat_ready_locked();
    unlock();
    return r;
}

esp_err_t voice_cloud_config_set(const cJSON *fields)
{
    if (!cJSON_IsObject(fields)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_vc.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    vc_config_t *next = vc_calloc_big(sizeof(*next));
    if (!next) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    memcpy(next, s_vc.cfg, sizeof(*next));
    unlock();

    bool present[6] = {false};
    int count = 0;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 6 && err == ESP_OK; i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(fields, k_fields[i]);
        if (!item) {
            continue;
        }
        const char *value = cJSON_IsNull(item) ? "" : (cJSON_IsString(item) ? item->valuestring : NULL);
        if (!value || !field_accept(i, value, field(next, i), field_size(i))) {
            err = ESP_ERR_INVALID_ARG; /* 不回显取值 */
            break;
        }
        present[i] = true;
        count++;
    }
    if (err != ESP_OK || count == 0) {
        wipe_free(next, sizeof(*next));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(VC_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        for (int i = 0; i < 6 && err == ESP_OK; i++) {
            if (!present[i]) {
                continue;
            }
            const char *v = field(next, i);
            if (v[0]) {
                err = nvs_set_str(h, k_fields[i], v);
            } else {
                err = nvs_erase_key(h, k_fields[i]);
                if (err == ESP_ERR_NVS_NOT_FOUND) {
                    err = ESP_OK;
                }
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config save failed: %s", esp_err_to_name(err));
        wipe_free(next, sizeof(*next));
        return err;
    }
    lock();
    memcpy(s_vc.cfg, next, sizeof(*next));
    bool asr = asr_ready_locked(), chat = chat_ready_locked();
    unlock();
    wipe_free(next, sizeof(*next));
    ESP_LOGI(TAG, "config saved: fields=%d asr_ready=%d chat_ready=%d", count, asr, chat);
    return ESP_OK;
}

cJSON *voice_cloud_config_public(void)
{
    char asr_model[VC_MODEL_MAX + 1] = VC_DEFAULT_ASR_MODEL;
    char chat_model[VC_MODEL_MAX + 1] = VC_DEFAULT_CHAT_MODEL;
    char asr_host[VC_HOST_MAX] = "";
    char chat_host[VC_HOST_MAX] = "";
    char base[VC_BASE_MAX + 1];
    bool asr = false, chat = false;
    vc_url_host(VC_DEFAULT_ASR_BASE, asr_host, sizeof(asr_host));
    vc_url_host(VC_DEFAULT_CHAT_BASE, chat_host, sizeof(chat_host));
    if (s_vc.inited) {
        lock();
        asr = asr_ready_locked();
        chat = chat_ready_locked();
        snprintf(asr_model, sizeof(asr_model), "%s", eff_asr_model(s_vc.cfg));
        snprintf(chat_model, sizeof(chat_model), "%s", eff_chat_model(s_vc.cfg));
        if (s_vc.cfg->asr_base[0] && vc_base_normalize(s_vc.cfg->asr_base, base, sizeof(base), true) == 0) {
            vc_url_host(base, asr_host, sizeof(asr_host));
        }
        if (s_vc.cfg->chat_base[0] && vc_base_normalize(s_vc.cfg->chat_base, base, sizeof(base), false) == 0) {
            vc_url_host(base, chat_host, sizeof(chat_host));
        }
        unlock();
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    /* 只输出状态、模型名与主机名，绝不包含密钥 */
    if (!cJSON_AddBoolToObject(o, "asr_ready", asr) || !cJSON_AddBoolToObject(o, "chat_ready", chat) ||
        !cJSON_AddStringToObject(o, "asr_model", asr_model) || !cJSON_AddStringToObject(o, "chat_model", chat_model) ||
        !cJSON_AddStringToObject(o, "asr_host", asr_host) || !cJSON_AddStringToObject(o, "chat_host", chat_host)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

esp_err_t voice_cloud_set_tools(const char *tools_json, voice_cloud_tool_cb_t cb, void *user_ctx)
{
    if (!s_vc.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    char *norm = NULL;
    if (tools_json && tools_json[0]) {
        if (!cb) {
            return ESP_ERR_INVALID_ARG;
        }
        norm = vc_tools_normalize(tools_json);
        if (!norm) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    lock();
    char *old = s_vc.tools_json;
    s_vc.tools_json = norm;
    s_vc.tool_cb = norm ? cb : NULL;
    s_vc.tool_ctx = norm ? user_ctx : NULL;
    unlock();
    free(old);
    return ESP_OK;
}

void voice_cloud_history_clear(void)
{
    if (!s_vc.inited) {
        return;
    }
    lock();
    for (size_t i = 0; i < s_vc.hist_n; i++) {
        free((char *)s_vc.hist[i].user);
        free((char *)s_vc.hist[i].assistant);
    }
    s_vc.hist_n = 0;
    unlock();
}

/* ---------------- 收包 ---------------- */

static void cancel_current(void)
{
    lock();
    if (s_vc.current) {
        atomic_store(&s_vc.current->cancelled, true);
    }
    unlock();
}

esp_err_t voice_cloud_begin(void)
{
    if (!s_vc.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *buf = vc_big_malloc(VC_STORE_CAP);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_store_mux);
    uint8_t *old = s_store.buf;
    s_store.buf = buf;
    s_store.cap = VC_STORE_CAP;
    s_store.used = 0;
    s_store.packets = 0;
    s_store.dropped = 0;
    s_store.last_packets = 0;
    s_store.receiving = true;
    portEXIT_CRITICAL(&s_store_mux);
    vc_big_free(old);
    cancel_current(); /* 开始新的一句：上一句的结果不再需要 */
    return ESP_OK;
}

esp_err_t voice_cloud_push_opus(const uint8_t *pkt, size_t len)
{
    if (!pkt || len == 0 || len > VC_OPUS_MAX_PACKET) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t r;
    portENTER_CRITICAL(&s_store_mux);
    if (!s_store.receiving || !s_store.buf) {
        r = ESP_ERR_INVALID_STATE;
    } else if (s_store.packets >= VC_MAX_PACKETS || s_store.used + 2 + len > s_store.cap) {
        s_store.dropped++; /* 超过 120 s 或缓存上限：丢弃 */
        r = ESP_ERR_INVALID_SIZE;
    } else {
        uint8_t *p = s_store.buf + s_store.used;
        p[0] = (uint8_t)len;
        p[1] = (uint8_t)(len >> 8);
        memcpy(p + 2, pkt, len);
        s_store.used += 2 + len;
        s_store.packets++;
        r = ESP_OK;
    }
    portEXIT_CRITICAL(&s_store_mux);
    return r;
}

size_t voice_cloud_packet_count(void)
{
    portENTER_CRITICAL(&s_store_mux);
    size_t n = s_store.receiving ? s_store.packets : s_store.last_packets;
    portEXIT_CRITICAL(&s_store_mux);
    return n;
}

/* ---------------- 会话 ---------------- */

static bool vc_job_cancelled(void *ctx)
{
    vc_session_t *s = ctx;
    return atomic_load(&s->cancelled);
}

static void vc_job_sleep(void *ctx, uint32_t ms)
{
    vc_session_t *s = ctx;
    while (ms && !atomic_load(&s->cancelled)) {
        uint32_t step = ms > 100 ? 100 : ms;
        TickType_t ticks = pdMS_TO_TICKS(step);
        vTaskDelay(ticks ? ticks : 1);
        ms -= step;
    }
}

static void vc_job_emit(void *ctx, voice_cloud_event_t evt, const char *text, const char *detail)
{
    vc_session_t *s = ctx;
    if (!atomic_load(&s->cancelled)) {
        s->cb(evt, text ? text : "", detail ? detail : "", s->cb_ctx);
    }
}

static void vc_job_release_frames(void *ctx)
{
    vc_session_t *s = ctx;
    vc_big_free(s->frames);
    s->frames = NULL;
    s->job.frames = NULL;
}

static void session_free(vc_session_t *s)
{
    if (!s) {
        return;
    }
    vc_job_release(&s->job);
    vc_secure_free(s->asr_key);
    vc_secure_free(s->chat_key);
    free(s->asr_context);
    vc_big_free(s->frames);
    free(s->text);
    free(s->tools_json);
    for (size_t i = 0; i < s->hist_n; i++) {
        free((char *)s->hist[i].user);
        free((char *)s->hist[i].assistant);
    }
    wipe_free(s, sizeof(*s));
}

static void make_now_text(char *out, size_t size)
{
    time_t t = time(NULL);
    struct tm tm;
    out[0] = '\0';
    if (!localtime_r(&t, &tm) || tm.tm_year + 1900 < 2024) {
        return; /* 时钟还没同步，不给模型错误的时间 */
    }
    static const char *const k_wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    snprintf(out, size, "%04d年%d月%d日 星期%s %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             k_wd[tm.tm_wday % 7], tm.tm_hour, tm.tm_min);
}

/* 在持锁状态下用当前配置、工具与历史生成会话快照 */
static vc_session_t *session_new_locked(voice_cloud_event_cb_t cb, void *cb_ctx, bool with_asr)
{
    vc_session_t *s = vc_calloc_big(sizeof(*s));
    if (!s) {
        return NULL;
    }
    atomic_init(&s->cancelled, false);
    s->cb = cb;
    s->cb_ctx = cb_ctx;
    const vc_config_t *c = s_vc.cfg;
    bool ok = true;
    vc_flavor_t flavor = VC_FLAVOR_OPENAI;
    snprintf(s->chat_model, sizeof(s->chat_model), "%s", eff_chat_model(c));
    ok = vc_chat_url(c->chat_base, c->asr_base, s->chat_model, s->chat_url, sizeof(s->chat_url), &flavor) == 0;
    s->chat_key = vc_strdup(c->chat_key);
    ok = ok && s->chat_key;
    if (with_asr) {
        snprintf(s->asr_model, sizeof(s->asr_model), "%s", eff_asr_model(c));
        ok = ok && vc_asr_url(c->asr_base, s->asr_url, sizeof(s->asr_url)) == 0;
        s->asr_key = vc_strdup(c->asr_key);
        s->asr_context = vc_strdup(s_vc.asr_context ? s_vc.asr_context : VC_ASR_CONTEXT);
        ok = ok && s->asr_key && s->asr_context;
    }
    if (ok && s_vc.tools_json) {
        s->tools_json = vc_strdup(s_vc.tools_json);
        ok = s->tools_json != NULL;
    }
    for (size_t i = 0; ok && i < s_vc.hist_n; i++) {
        s->hist[i].user = vc_strdup(s_vc.hist[i].user);
        s->hist[i].assistant = vc_strdup(s_vc.hist[i].assistant);
        s->hist_n = i + 1;
        ok = s->hist[i].user && s->hist[i].assistant;
    }
    if (!ok) {
        session_free(s);
        return NULL;
    }
    make_now_text(s->now_text, sizeof(s->now_text));

    vc_job_t *j = &s->job;
    j->asr_url = s->asr_url;
    j->asr_key = s->asr_key;
    j->asr_model = s->asr_model;
    j->chat_url = s->chat_url;
    j->chat_key = s->chat_key;
    j->chat_model = s->chat_model;
    j->flavor = flavor;
    j->asr_context = s->asr_context;
    j->tools_json = s->tools_json;
    j->tool_cb = s->tools_json ? s_vc.tool_cb : NULL;
    j->tool_ctx = s_vc.tool_ctx;
    j->history = s->hist;
    j->history_n = s->hist_n;
    j->now_text = s->now_text[0] ? s->now_text : NULL;
    j->tp = vc_platform_transport();
    j->emit = vc_job_emit;
    j->emit_ctx = s;
    j->cancelled = vc_job_cancelled;
    j->cancel_ctx = s;
    j->now_ms = vc_now_ms;
    j->sleep_ms = vc_job_sleep;
    j->env_ctx = s;
    j->release_frames = vc_job_release_frames;
    j->release_ctx = s;
    return s;
}

/* 把本轮问答写进历史（接管字符串所有权），只保留最近 4 轮 */
static void history_push_locked(char *user, char *assistant)
{
    if (s_vc.hist_n == VC_HISTORY_TURNS) {
        free((char *)s_vc.hist[0].user);
        free((char *)s_vc.hist[0].assistant);
        memmove(&s_vc.hist[0], &s_vc.hist[1], sizeof(s_vc.hist[0]) * (VC_HISTORY_TURNS - 1));
        s_vc.hist_n--;
    }
    size_t n = strlen(user);
    if (n > VC_HIST_USER_MAX) {
        user[vc_utf8_cut(user, n, VC_HIST_USER_MAX)] = '\0';
    }
    s_vc.hist[s_vc.hist_n].user = user;
    s_vc.hist[s_vc.hist_n].assistant = assistant;
    s_vc.hist_n++;
}

static void vc_worker(void *arg)
{
    vc_session_t *s = arg;
    vc_run_t rc = vc_pipeline_run(&s->job);

    /* 先写历史并清除忙状态，再发终态回调：回调里可以直接开始下一句 */
    lock();
    if (rc == VC_RUN_OK && !atomic_load(&s->cancelled) && s->job.out_user && s->job.out_reply) {
        history_push_locked(s->job.out_user, s->job.out_reply);
        s->job.out_user = NULL;
        s->job.out_reply = NULL;
    }
    if (s_vc.current == s) {
        s_vc.current = NULL;
    }
    unlock();

    if (rc != VC_RUN_CANCELLED && !atomic_load(&s->cancelled)) {
        s->cb(s->job.final.evt, s->job.final.text ? s->job.final.text : "", s->job.final.detail, s->cb_ctx);
    }
    UBaseType_t free_stack = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "session end: rc=%d %lld ms stack_free=%u bytes stack=%s", (int)rc,
             (long long)(vc_now_ms(NULL) - s->started_ms), (unsigned)free_stack, s->stack_psram ? "psram" : "internal");
    session_free(s);

    lock();
    s_vc.live--;
    unlock();
    vTaskDeleteWithCaps(NULL);
}

/* 启动工作任务；调用前 s 已登记为 current、live 已加一（失败时由本函数回滚） */
static esp_err_t start_worker(vc_session_t *s)
{
    s->started_ms = vc_now_ms(NULL);
    UBaseType_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if VOICE_CLOUD_TASK_STACK_PSRAM
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= VOICE_CLOUD_TASK_STACK) {
        caps = MALLOC_CAP_SPIRAM;
    }
#endif
    s->stack_psram = caps == MALLOC_CAP_SPIRAM;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(vc_worker, "voice_cloud", VOICE_CLOUD_TASK_STACK, s,
                                                    VOICE_CLOUD_TASK_PRIO, NULL, tskNO_AFFINITY, caps);
    if (ok != pdPASS) {
        /* 换另一种内存再试一次 */
        caps = s->stack_psram ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : MALLOC_CAP_SPIRAM;
        s->stack_psram = caps == MALLOC_CAP_SPIRAM;
        ok = xTaskCreatePinnedToCoreWithCaps(vc_worker, "voice_cloud", VOICE_CLOUD_TASK_STACK, s,
                                             VOICE_CLOUD_TASK_PRIO, NULL, tskNO_AFFINITY, caps);
    }
    if (ok == pdPASS) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "worker task create failed: stack=%u internal_largest=%u psram_largest=%u",
             (unsigned)VOICE_CLOUD_TASK_STACK, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    lock();
    if (s_vc.current == s) {
        s_vc.current = NULL;
    }
    s_vc.live--;
    unlock();
    session_free(s);
    return ESP_ERR_NO_MEM;
}

esp_err_t voice_cloud_finish(voice_cloud_event_cb_t cb, void *user_ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_vc.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (busy_locked() || s_vc.live >= 2) {
        unlock();
        return ESP_ERR_INVALID_STATE; /* 上一句还在处理；录音保留，可稍后再 finish */
    }
    portENTER_CRITICAL(&s_store_mux);
    bool receiving = s_store.receiving;
    uint8_t *buf = s_store.buf;
    size_t used = s_store.used, packets = s_store.packets, dropped = s_store.dropped;
    if (receiving) {
        s_store.buf = NULL;
        s_store.receiving = false;
        s_store.used = 0;
        s_store.packets = 0;
        s_store.last_packets = packets;
    }
    portEXIT_CRITICAL(&s_store_mux);
    if (!receiving) {
        unlock();
        return ESP_ERR_INVALID_STATE; /* 没有 begin */
    }
    bool configured = asr_ready_locked() && chat_ready_locked();
    vc_session_t *s = NULL;
    if (configured && packets >= VC_MIN_PACKETS) {
        s = session_new_locked(cb, user_ctx, true);
        if (s) {
            s->frames = buf;
            s->frames_len = used;
            s->packets = packets;
            s->job.frames = buf;
            s->job.frames_len = used;
            buf = NULL;
            s_vc.current = s;
            s_vc.live++;
        }
    }
    unlock();

    if (!configured) {
        vc_big_free(buf);
        cb(VOICE_CLOUD_EVT_ERROR, VC_MSG_NOT_CONFIGURED, "not_configured", user_ctx);
        return ESP_OK;
    }
    if (packets < VC_MIN_PACKETS) {
        vc_big_free(buf);
        cb(VOICE_CLOUD_EVT_ERROR, VC_MSG_NO_AUDIO, "no_audio", user_ctx);
        return ESP_OK;
    }
    if (!s) {
        vc_big_free(buf);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "finish: packets=%u bytes=%u dropped=%u", (unsigned)packets, (unsigned)used, (unsigned)dropped);
    return start_worker(s);
}

esp_err_t voice_cloud_ask_text(const char *text, voice_cloud_event_cb_t cb, void *user_ctx)
{
    if (!cb || !text) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(text);
    if (!vc_utf8_valid(text, n)) {
        return ESP_ERR_INVALID_ARG;
    }
    while (n && (text[0] == ' ' || text[0] == '\n' || text[0] == '\t' || text[0] == '\r')) {
        text++;
        n--;
    }
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\n' || text[n - 1] == '\t' || text[n - 1] == '\r')) {
        n--;
    }
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_vc.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    n = vc_utf8_cut(text, n, VC_TEXT_IN_MAX);
    char *copy = malloc(n + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, text, n);
    copy[n] = '\0';

    lock();
    if (busy_locked() || s_vc.live >= 2) {
        unlock();
        free(copy);
        return ESP_ERR_INVALID_STATE;
    }
    bool configured = chat_ready_locked();
    vc_session_t *s = configured ? session_new_locked(cb, user_ctx, false) : NULL;
    if (s) {
        s->text = copy;
        s->job.text = copy;
        copy = NULL;
        s_vc.current = s;
        s_vc.live++;
    }
    unlock();

    if (!configured) {
        free(copy);
        cb(VOICE_CLOUD_EVT_ERROR, VC_MSG_NOT_CONFIGURED, "not_configured", user_ctx);
        return ESP_OK;
    }
    if (!s) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ask_text: %u bytes", (unsigned)n);
    return start_worker(s);
}

void voice_cloud_cancel(void)
{
    portENTER_CRITICAL(&s_store_mux);
    uint8_t *buf = s_store.buf;
    s_store.buf = NULL;
    s_store.receiving = false;
    s_store.used = 0;
    s_store.packets = 0;
    s_store.last_packets = 0;
    portEXIT_CRITICAL(&s_store_mux);
    vc_big_free(buf);
    if (s_vc.inited) {
        cancel_current();
    }
}

bool voice_cloud_busy(void)
{
    if (!s_vc.inited) {
        return false;
    }
    lock();
    bool b = busy_locked();
    unlock();
    return b;
}

/* ---------------- 测试/核验用的内部入口 ---------------- */

void vc_internal_set_asr_context(const char *context)
{
    if (!s_vc.inited) {
        return;
    }
    char *copy = context ? vc_strdup(context) : NULL;
    lock();
    char *old = s_vc.asr_context;
    s_vc.asr_context = copy;
    unlock();
    free(old);
}

int vc_internal_live_workers(void)
{
    if (!s_vc.inited) {
        return 0;
    }
    lock();
    int n = s_vc.live;
    unlock();
    return n;
}

void vc_internal_reset(void)
{
    if (!s_vc.inited) {
        return;
    }
    voice_cloud_cancel();
    for (int i = 0; i < 1000 && vc_internal_live_workers() > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    voice_cloud_history_clear();
    lock();
    free(s_vc.tools_json);
    s_vc.tools_json = NULL;
    s_vc.tool_cb = NULL;
    s_vc.tool_ctx = NULL;
    free(s_vc.asr_context);
    s_vc.asr_context = NULL;
    wipe_free(s_vc.cfg, sizeof(*s_vc.cfg));
    s_vc.cfg = NULL;
    s_vc.inited = false;
    unlock();
}

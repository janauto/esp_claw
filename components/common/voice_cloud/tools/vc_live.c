/*
 * 宿主上的真实云端核验驱动：用组件的公开 API（voice_cloud_init / config_set / set_tools / begin /
 * push_opus / finish / ask_text）跑完整链路，HTTP 换成 libcurl，FreeRTOS/NVS 用 test_host/stubs 的桩
 * （NVS 只在进程内存里，不落盘）。由 live_check.py 编译运行。
 *
 *   vc_live --env ~/.config/backclip-voice/secrets.env (--frames x.frames | --text "…")
 *           [--dump DIR] [--no-context] [--asr-only] [--chat-dashscope MODEL]
 *           [--chat-base URL --chat-model M --chat-key-var VAR]
 *
 * 密钥只从 env 文件读进内存交给组件，绝不打印、不写文件；--dump 只写请求正文（正文里没有密钥）。
 * 标准输出是 JSON 行：事件、每次 HTTP 的状态/耗时/字节数、最后一行汇总。
 */
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vc_internal.h"
#include "voice_cloud.h"

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t g_t0;
static const char *g_dump;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_done;
static int g_req;
static bool g_asr_only;

/* JSON 字符串输出（用 cJSON 转义） */
static void print_kv_event(const char *event, const char *text, const char *detail)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "t_ms", (double)(mono_ms() - g_t0));
    cJSON_AddStringToObject(o, "event", event);
    cJSON_AddStringToObject(o, "text", text ? text : "");
    cJSON_AddStringToObject(o, "detail", detail ? detail : "");
    char *s = cJSON_PrintUnformatted(o);
    printf("%s\n", s);
    fflush(stdout);
    cJSON_free(s);
    cJSON_Delete(o);
}

/* 只保留域名最后两段，其余打码（业务空间专属域名不外泄） */
static void mask_host(const char *url, char *out, size_t size)
{
    char host[VC_HOST_MAX] = "?";
    vc_url_host(url, host, sizeof(host));
    const char *last = strrchr(host, '.');
    const char *second = NULL;
    for (const char *p = host; last && p < last; p++) {
        if (*p == '.') {
            second = p;
        }
    }
    if (second) {
        snprintf(out, size, "*%s", second);
    } else {
        snprintf(out, size, "%s", host);
    }
}

/* ---------------- libcurl 传输 ---------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t max;
    bool overflow;
} acc_t;

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    acc_t *a = ud;
    size_t n = size * nmemb;
    if (a->len + n > a->max) {
        a->overflow = true;
        return 0;
    }
    char *nb = vc_big_realloc(a->buf, a->len + n + 1);
    if (!nb) {
        return 0;
    }
    a->buf = nb;
    memcpy(a->buf + a->len, ptr, n);
    a->len += n;
    a->buf[a->len] = '\0';
    return n;
}

static vc_net_t curl_post(void *ctx, const char *url, const char *key, const char *body, size_t body_len,
                          int timeout_ms, size_t max_resp, vc_http_resp_t *resp)
{
    (void)ctx;
    memset(resp, 0, sizeof(*resp));
    pthread_mutex_lock(&g_mu);
    int idx = g_req++;
    pthread_mutex_unlock(&g_mu);
    if (g_dump) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/req_%d.json", g_dump, idx);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(body, 1, body_len, f);
            fclose(f);
        }
    }
    CURL *c = curl_easy_init();
    if (!c) {
        return VC_NET_NOMEM;
    }
    size_t klen = strlen(key);
    char *auth = malloc(klen + 32);
    snprintf(auth, klen + 32, "Authorization: Bearer %s", key);
    struct curl_slist *h = curl_slist_append(NULL, auth);
    memset(auth, 0, klen + 32);
    free(auth);
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, "Accept: application/json");
    acc_t acc = {.max = max_resp};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_ms < 10000 ? timeout_ms : 10000));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &acc);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "PisenAIClip/voice-cloud-host");
    int64_t t = mono_ms();
    CURLcode rc = curl_easy_perform(c);
    int64_t spent = mono_ms() - t;
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    double up = 0, connect = 0, tls = 0, first = 0;
    curl_easy_getinfo(c, CURLINFO_CONNECT_TIME, &connect);
    curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &tls);
    curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &first);
    curl_easy_getinfo(c, CURLINFO_SPEED_UPLOAD, &up);
    /* 清掉 slist 里带密钥的那一行再释放 */
    for (struct curl_slist *p = h; p; p = p->next) {
        memset(p->data, 0, strlen(p->data));
    }
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    char host[VC_HOST_MAX];
    mask_host(url, host, sizeof(host));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "t_ms", (double)(mono_ms() - g_t0));
    cJSON_AddStringToObject(o, "event", "HTTP");
    /* 百炼对话与 ASR 可能是同一个地址，按正文里有没有音频区分阶段 */
    cJSON_AddStringToObject(o, "stage", strstr(body, "\"input_audio\"") ? "asr" : "chat");
    cJSON_AddStringToObject(o, "host", host);
    cJSON_AddNumberToObject(o, "status", (double)status);
    cJSON_AddNumberToObject(o, "curl", (double)rc);
    cJSON_AddNumberToObject(o, "ms", (double)spent);
    cJSON_AddNumberToObject(o, "tls_ms", tls * 1000.0);
    cJSON_AddNumberToObject(o, "ttfb_ms", first * 1000.0);
    cJSON_AddNumberToObject(o, "req_bytes", (double)body_len);
    cJSON_AddNumberToObject(o, "resp_bytes", (double)acc.len);
    cJSON_AddNumberToObject(o, "timeout_ms", timeout_ms);
    char *s = cJSON_PrintUnformatted(o);
    printf("%s\n", s);
    fflush(stdout);
    cJSON_free(s);
    cJSON_Delete(o);

    if (rc != CURLE_OK) {
        vc_big_free(acc.buf);
        if (acc.overflow) {
            return VC_NET_TOO_LARGE;
        }
        return rc == CURLE_OPERATION_TIMEDOUT ? VC_NET_TIMEOUT : VC_NET_FAIL;
    }
    resp->status = (int)status;
    resp->body = acc.buf;
    resp->body_len = acc.len;
    return VC_NET_OK;
}

static const vc_transport_t s_curl = {.post = curl_post, .ctx = NULL};

const vc_transport_t *vc_platform_transport(void)
{
    return &s_curl;
}

/* ---------------- 事件与工具 ---------------- */

static const char *evt_name(voice_cloud_event_t e)
{
    switch (e) {
    case VOICE_CLOUD_EVT_RECOGNIZING: return "RECOGNIZING";
    case VOICE_CLOUD_EVT_HEARD: return "HEARD";
    case VOICE_CLOUD_EVT_THINKING: return "THINKING";
    case VOICE_CLOUD_EVT_TOOL: return "TOOL";
    case VOICE_CLOUD_EVT_REPLY: return "REPLY";
    default: return "ERROR";
    }
}

static void on_event(voice_cloud_event_t evt, const char *text, const char *detail, void *ctx)
{
    (void)ctx;
    print_kv_event(evt_name(evt), text, detail);
    if (evt == VOICE_CLOUD_EVT_HEARD && g_asr_only) {
        voice_cloud_cancel(); /* 只看识别结果：取消后续对话 */
        pthread_mutex_lock(&g_mu);
        g_done = 1;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    if (evt == VOICE_CLOUD_EVT_REPLY || evt == VOICE_CLOUD_EVT_ERROR) {
        pthread_mutex_lock(&g_mu);
        g_done = 1;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
}

/* 假的设备执行器：回一个模型能理解的结果 */
static esp_err_t on_tool(const char *name, const cJSON *args, char *out, size_t out_size, void *ctx)
{
    (void)ctx;
    const cJSON *percent = cJSON_GetObjectItem(args, "percent");
    const cJSON *mode = cJSON_GetObjectItem(args, "mode");
    const cJSON *app = cJSON_GetObjectItem(args, "name");
    if (strcmp(name, "set_brightness") == 0 && cJSON_IsNumber(percent)) {
        snprintf(out, out_size, "{\"brightness\":%d}", percent->valueint);
    } else if (strcmp(name, "set_volume") == 0 && cJSON_IsNumber(percent)) {
        snprintf(out, out_size, "{\"volume\":%d}", percent->valueint);
    } else if (strcmp(name, "set_mode") == 0 && cJSON_IsString(mode)) {
        snprintf(out, out_size, "{\"mode\":\"%s\"}", mode->valuestring);
    } else if (strcmp(name, "open_app") == 0 && cJSON_IsString(app)) {
        snprintf(out, out_size, "{\"launched\":true}");
    } else {
        snprintf(out, out_size, "参数不对");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ---------------- env 文件 ---------------- */

static char *env_get(const char *path, const char *name)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    char line[2048];
    char *value = NULL;
    size_t nlen = strlen(name);
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "export ", 7) == 0) {
            p += 7;
        }
        if (strncmp(p, name, nlen) != 0 || p[nlen] != '=') {
            continue;
        }
        p += nlen + 1;
        size_t n = strcspn(p, "\r\n");
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
            n--;
        }
        if (n >= 2 && (p[0] == '"' || p[0] == '\'') && p[n - 1] == p[0]) {
            p++;
            n -= 2;
        }
        free(value);
        value = strndup(p, n);
    }
    memset(line, 0, sizeof(line));
    fclose(f);
    return value;
}

static void wait_done(int timeout_s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_s;
    pthread_mutex_lock(&g_mu);
    while (!g_done && pthread_cond_timedwait(&g_cv, &g_mu, &ts) == 0) {
    }
    pthread_mutex_unlock(&g_mu);
}

int main(int argc, char **argv)
{
    const char *env = NULL, *frames = NULL, *text = NULL, *chat_base = NULL, *chat_model = NULL;
    const char *chat_key_var = "DEEPSEEK_API_KEY";
    bool no_context = false;
    bool chat_dashscope = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--env") && i + 1 < argc) env = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump = argv[++i];
        else if (!strcmp(argv[i], "--chat-base") && i + 1 < argc) chat_base = argv[++i];
        else if (!strcmp(argv[i], "--chat-model") && i + 1 < argc) chat_model = argv[++i];
        else if (!strcmp(argv[i], "--chat-key-var") && i + 1 < argc) chat_key_var = argv[++i];
        else if (!strcmp(argv[i], "--no-context")) no_context = true;
        else if (!strcmp(argv[i], "--asr-only")) g_asr_only = true;
        else if (!strcmp(argv[i], "--chat-dashscope") && i + 1 < argc) {
            /* 对话也走百炼兼容模式：根地址与密钥取自 env 文件，不经命令行 */
            chat_dashscope = true;
            chat_model = argv[++i];
            chat_key_var = "DASHSCOPE_API_KEY";
        }
        else {
            fprintf(stderr, "bad argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!env || (!frames && !text)) {
        fprintf(stderr, "usage: vc_live --env FILE (--frames F | --text T) [--dump DIR]\n");
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_t0 = mono_ms();

    char *asr_key = env_get(env, "DASHSCOPE_API_KEY");
    char *asr_base = env_get(env, "DASHSCOPE_BASE_URL");
    char *chat_key = env_get(env, chat_key_var);
    if (!asr_key || !chat_key) {
        fprintf(stderr, "missing keys in env file\n"); /* 不说具体值 */
        return 2;
    }
    if (voice_cloud_init() != ESP_OK) {
        return 3;
    }
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "asr_key", asr_key);
    cJSON_AddStringToObject(cfg, "chat_key", chat_key);
    if (asr_base && asr_base[0]) {
        cJSON_AddStringToObject(cfg, "asr_base", asr_base);
    }
    if (chat_dashscope) {
        cJSON_AddStringToObject(cfg, "chat_base", asr_base && asr_base[0] ? asr_base : VC_DEFAULT_ASR_BASE);
    } else if (chat_base) {
        cJSON_AddStringToObject(cfg, "chat_base", chat_base);
    }
    if (chat_model) {
        cJSON_AddStringToObject(cfg, "chat_model", chat_model);
    }
    esp_err_t err = voice_cloud_config_set(cfg);
    /* 清掉本地副本：cJSON 里的字符串与 env 读出的值 */
    cJSON *it;
    cJSON_ArrayForEach(it, cfg) {
        if (cJSON_IsString(it)) {
            memset(it->valuestring, 0, strlen(it->valuestring));
        }
    }
    cJSON_Delete(cfg);
    vc_secure_free(asr_key);
    vc_secure_free(chat_key);
    free(asr_base);
    if (err != ESP_OK) {
        fprintf(stderr, "config_set failed: %s\n", esp_err_to_name(err));
        return 3;
    }
    cJSON *pub = voice_cloud_config_public();
    char *pubs = cJSON_PrintUnformatted(pub);
    /* 公开配置本身不含密钥；主机名打码后再打印 */
    cJSON *masked = cJSON_Parse(pubs);
    char h1[VC_HOST_MAX], h2[VC_HOST_MAX], u[VC_URL_MAX];
    snprintf(u, sizeof(u), "https://%s/", cJSON_GetObjectItem(pub, "asr_host")->valuestring);
    mask_host(u, h1, sizeof(h1));
    snprintf(u, sizeof(u), "https://%s/", cJSON_GetObjectItem(pub, "chat_host")->valuestring);
    mask_host(u, h2, sizeof(h2));
    cJSON_ReplaceItemInObject(masked, "asr_host", cJSON_CreateString(h1));
    cJSON_ReplaceItemInObject(masked, "chat_host", cJSON_CreateString(h2));
    char *ms = cJSON_PrintUnformatted(masked);
    printf("{\"event\":\"CONFIG\",\"public\":%s}\n", ms);
    cJSON_free(ms);
    cJSON_Delete(masked);
    cJSON_free(pubs);
    cJSON_Delete(pub);

    if (no_context) {
        vc_internal_set_asr_context("");
    }
    if (voice_cloud_set_tools(voice_cloud_default_tools_json(), on_tool, NULL) != ESP_OK) {
        return 3;
    }

    int64_t start = mono_ms();
    g_t0 = start;
    size_t packets = 0;
    if (frames) {
        FILE *f = fopen(frames, "rb");
        if (!f) {
            return 2;
        }
        voice_cloud_begin();
        uint8_t hdr[2];
        uint8_t pkt[VC_OPUS_MAX_PACKET];
        while (fread(hdr, 1, 2, f) == 2) {
            size_t n = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
            if (n == 0 || n > sizeof(pkt) || fread(pkt, 1, n, f) != n) {
                break;
            }
            if (voice_cloud_push_opus(pkt, n) == ESP_OK) {
                packets++;
            }
        }
        fclose(f);
        start = mono_ms();
        g_t0 = start; /* 事件时间从“松开按键”（finish）算起 */
        err = voice_cloud_finish(on_event, NULL);
    } else {
        err = voice_cloud_ask_text(text, on_event, NULL);
    }
    if (err != ESP_OK) {
        fprintf(stderr, "start failed: %s\n", esp_err_to_name(err));
        return 3;
    }
    wait_done(90);
    int64_t total = mono_ms() - start;
    for (int i = 0; i < 300 && vc_internal_live_workers() > 0; i++) {
        struct timespec ts = {0, 10 * 1000000L};
        nanosleep(&ts, NULL);
    }
    printf("{\"event\":\"SUMMARY\",\"mode\":\"%s\",\"packets\":%zu,\"audio_ms\":%zu,\"total_ms\":%lld,"
           "\"requests\":%d,\"done\":%d}\n",
           frames ? "voice" : "text", packets, packets * VC_FRAME_MS, (long long)total, g_req, g_done);
    vc_internal_reset();
    curl_global_cleanup();
    return g_done ? 0 : 4;
}

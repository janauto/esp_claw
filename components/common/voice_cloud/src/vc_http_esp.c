/*
 * 设备端 HTTPS 传输：esp_http_client + 证书包，做法与 live_http.c 一致。
 *
 * - keep_alive_enable=true：交给 components/common/http_reuse 的连接池（--wrap 钩子），
 *   同一会话里 ASR → 对话、以及工具多轮请求可复用 TLS 连接；
 * - 请求正文由调用者放在 PSRAM，这里只设置指针（esp_http_client_set_post_field 不拷贝）；
 * - 响应体按需在 PSRAM 里增长，超过上限即判为 TOO_LARGE；
 * - 关闭自动重定向与授权重试，避免把 Authorization 头带到别的主机；
 * - 归还连接前删掉 Authorization 头，池里的句柄不会留着本组件的密钥。
 * 日志只记状态码、字节数与耗时。
 */
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "vc_internal.h"

static const char *TAG = "voice_cloud";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t max;
    bool overflow;
    bool nomem;
} vc_acc_t;

static esp_err_t vc_http_event(esp_http_client_event_t *evt)
{
    vc_acc_t *a = evt->user_data;
    if (!a) {
        return ESP_OK;
    }
    switch (evt->event_id) {
    case HTTP_EVENT_HEADERS_SENT:
        /* 每次（含连接池内部的一次自动重试）发送请求后重新累计响应 */
        a->len = 0;
        a->overflow = false;
        if (a->buf) {
            a->buf[0] = '\0';
        }
        break;
    case HTTP_EVENT_ON_DATA: {
        if (a->overflow || a->nomem || evt->data_len <= 0) {
            break;
        }
        size_t n = (size_t)evt->data_len;
        if (a->len + n > a->max) {
            a->overflow = true;
            break;
        }
        if (a->len + n + 1 > a->cap) {
            size_t cap = a->cap ? a->cap : 2048;
            while (cap < a->len + n + 1) {
                cap *= 2;
            }
            if (cap > a->max + 1) {
                cap = a->max + 1;
            }
            char *nb = vc_big_realloc(a->buf, cap);
            if (!nb) {
                a->nomem = true;
                break;
            }
            a->buf = nb;
            a->cap = cap;
        }
        memcpy(a->buf + a->len, evt->data, n);
        a->len += n;
        a->buf[a->len] = '\0';
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

static vc_net_t vc_esp_post(void *ctx, const char *url, const char *key, const char *body, size_t body_len,
                            int timeout_ms, size_t max_resp, vc_http_resp_t *resp)
{
    (void)ctx;
    memset(resp, 0, sizeof(*resp));
    vc_acc_t acc = {.max = max_resp};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = vc_http_event,
        .user_data = &acc,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return VC_NET_NOMEM;
    }
    /* Authorization 头：拼好后立即交给 client（内部会复制），本地副本清零释放 */
    size_t klen = strlen(key);
    char *auth = malloc(klen + 8);
    esp_err_t err = auth ? ESP_OK : ESP_ERR_NO_MEM;
    if (auth) {
        memcpy(auth, "Bearer ", 7);
        memcpy(auth + 7, key, klen + 1);
        err = esp_http_client_set_header(client, "Authorization", auth);
        vc_secure_free(auth);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Accept", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "User-Agent", "PisenAIClip/voice-cloud-1");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(client, body, (int)body_len);
    }
    int64_t t0 = esp_timer_get_time();
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    int64_t spent_ms = (esp_timer_get_time() - t0) / 1000;
    int status = esp_http_client_get_status_code(client);

    /* 归还连接池前清理：不留密钥、不留指向已释放正文的指针、不留本地栈上的 user_data */
    esp_http_client_delete_header(client, "Authorization");
    esp_http_client_set_post_field(client, NULL, 0);
    esp_http_client_set_user_data(client, NULL);
    esp_http_client_cleanup(client);

    ESP_LOGD(TAG, "post: err=%s status=%d req=%u resp=%u %lld ms", esp_err_to_name(err), status,
             (unsigned)body_len, (unsigned)acc.len, (long long)spent_ms);
    if (err != ESP_OK) {
        vc_big_free(acc.buf);
        if (err == ESP_ERR_NO_MEM) {
            return VC_NET_NOMEM;
        }
        if (err == ESP_ERR_HTTP_READ_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN || spent_ms + 200 >= timeout_ms) {
            return VC_NET_TIMEOUT;
        }
        return VC_NET_FAIL;
    }
    resp->status = status;
    if (acc.overflow) {
        vc_big_free(acc.buf);
        return VC_NET_TOO_LARGE;
    }
    if (acc.nomem) {
        vc_big_free(acc.buf);
        return VC_NET_NOMEM;
    }
    resp->body = acc.buf;
    resp->body_len = acc.len;
    return VC_NET_OK;
}

static const vc_transport_t s_esp_transport = {.post = vc_esp_post, .ctx = NULL};

const vc_transport_t *vc_platform_transport(void)
{
    return &s_esp_transport;
}

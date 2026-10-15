/*
 * voice_cloud 内部接口。
 *
 * 纯逻辑（vc_ogg.c / vc_text.c / vc_proto.c / vc_pipeline.c）不依赖 ESP-IDF，可在宿主编译测试；
 * 设备胶水（voice_cloud.c：NVS、收包缓存、工作任务）与 HTTP 传输（vc_http_esp.c）只在固件里编译，
 * 宿主测试用 test_host/stubs 里的桩函数替代 FreeRTOS/NVS。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_err.h"
#include "voice_cloud.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 固定参数 ---------------- */

#define VC_FRAME_MS             60      /* 背夹每包 60 ms */
#define VC_SAMPLES_PER_PACKET   2880    /* Ogg granule 按 48 kHz 计：60 ms × 48 */
#define VC_OPUS_PRE_SKIP        312     /* libopus 常规前导样本数（48 kHz） */
#define VC_OPUS_INPUT_RATE      16000   /* OpusHead 里的原始采样率 */
#define VC_OGG_SERIAL           0x42435650u /* "BCVP"，与语音中继 ogg_opus.py 相同 */
#define VC_OGG_PACKETS_PER_PAGE 16
#define VC_OGG_VENDOR           "backclip-voice-cloud"

#define VC_MAX_PACKETS          2000    /* 120 s / 60 ms */
#define VC_OPUS_MAX_PACKET      1275    /* 单个 Opus 包上限（与手机中继的检查一致） */
#define VC_STORE_CAP            (512u * 1024u) /* 收包缓存上限（PSRAM），够 120 s × 约 34 kbps */
#define VC_MIN_PACKETS          3       /* 少于 180 ms 视为没录到声音 */

#define VC_BASE_MAX             200     /* 服务根地址最大长度 */
#define VC_KEY_MAX              256     /* 密钥最大长度 */
#define VC_MODEL_MAX            64      /* 模型名最大长度 */
#define VC_URL_MAX              (VC_BASE_MAX + 48)
#define VC_HOST_MAX             128

#define VC_HISTORY_TURNS        4       /* 保留最近 4 轮 */
#define VC_TOOL_ROUNDS          2       /* 最多 2 轮工具调用 */
#define VC_TOOL_CALLS_PER_ROUND 4       /* 每轮最多执行的工具调用数，多出的回“未执行” */
#define VC_TOOL_OUT_MAX         512     /* 工具回调输出缓冲 */
#define VC_TOOLS_MAX            16      /* 最多注入的工具数 */
#define VC_TOOLS_JSON_MAX       8192
#define VC_TEXT_IN_MAX          1000    /* ask_text 输入上限（字节，按 UTF-8 截断） */
#define VC_REPLY_MAX_BYTES      300     /* 回复截断上限（字节，含省略号） */
#define VC_HEARD_MAX_BYTES      240     /* “听到”文字截断上限 */

#define VC_ASR_BUDGET_MS        15000   /* ASR 阶段基础时限 */
#define VC_ASR_BUDGET_MAX_MS    30000   /* 长录音时最多放宽到 30 s */
#define VC_CHAT_BUDGET_MS       25000   /* 对话阶段（含工具与重试）总时限 */
#define VC_HTTP_OP_MAX_MS       15000   /* 单次 HTTP 的网络超时上限 */
#define VC_HTTP_MIN_MS          1500    /* 剩余时间少于此值不再发起新请求 */
#define VC_RETRY_MAX            2       /* 503/429/5xx/网络错误最多重试 2 次 */
#define VC_RETRY_BASE_MS        600     /* 退避：600 ms、1200 ms */
#define VC_ASR_RESP_MAX         (16u * 1024u)
#define VC_CHAT_RESP_MAX        (48u * 1024u)
#define VC_CHAT_MAX_TOKENS      400

#define VC_DEFAULT_ASR_BASE     "https://dashscope.aliyuncs.com"
#define VC_DEFAULT_ASR_MODEL    "qwen3-asr-flash"
#define VC_DEFAULT_CHAT_BASE    "https://api.deepseek.com"
#define VC_DEFAULT_CHAT_MODEL   "deepseek-flash"

/* 热词上下文：没有它“背夹”会被识别成“背佳”（见 voice-asr/VALIDATION.md）。 */
#define VC_ASR_CONTEXT          "背夹 磁吸背夹 品胜 小磁 底座 亮度 音量 Lua LVGL ESP-Claw WorkBuddy"

/* ---------------- 日志（只记字节数、耗时、状态码，绝不记密钥与正文） ---------------- */

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define VC_LOGI(fmt, ...) ESP_LOGI("voice_cloud", fmt, ##__VA_ARGS__)
#define VC_LOGW(fmt, ...) ESP_LOGW("voice_cloud", fmt, ##__VA_ARGS__)
#else
void vc_host_log(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define VC_LOGI(fmt, ...) vc_host_log('I', fmt, ##__VA_ARGS__)
#define VC_LOGW(fmt, ...) vc_host_log('W', fmt, ##__VA_ARGS__)
#endif

/* ---------------- 大块内存：设备上优先 PSRAM ---------------- */

void *vc_big_malloc(size_t size);
void *vc_big_realloc(void *ptr, size_t size);
void vc_big_free(void *ptr);
char *vc_strdup(const char *s);            /* 普通小字符串拷贝，s 为 NULL 时返回 NULL */
void vc_secure_free(char *s);              /* 先清零再释放（用于密钥副本） */

/* ---------------- 收包格式与 Ogg Opus 封装（vc_ogg.c） ---------------- */

/* 收包缓存格式与手机上送的 application/x-opus-frames 相同：重复 [u16 小端长度][Opus 包]。 */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} vc_frames_iter_t;

void vc_frames_iter_init(vc_frames_iter_t *it, const uint8_t *data, size_t len);
/* 返回 1 = 取到一包，0 = 结束，-1 = 格式错误 */
int vc_frames_next(vc_frames_iter_t *it, const uint8_t **pkt, size_t *pkt_len);
/* 统计包数，格式错误返回 -1 */
long vc_frames_count(const uint8_t *data, size_t len);

typedef struct {
    uint32_t serial;
    uint16_t pre_skip;
    uint32_t input_rate;
    uint8_t channels;
    uint32_t samples_per_packet;
    uint8_t packets_per_page;
    const char *vendor;
} vc_ogg_opts_t;

void vc_ogg_opts_default(vc_ogg_opts_t *o);
/* Ogg CRC32：多项式 0x04C11DB7、不反射、初值 0（与 libogg 一致）；可分段累计 */
uint32_t vc_ogg_crc(uint32_t crc, const uint8_t *data, size_t len);

typedef int (*vc_sink_fn)(void *ctx, const uint8_t *data, size_t len);
/* 逐页输出 Ogg Opus 字节流（OpusHead 页、OpusTags 页、音频页），sink 返回非 0 即中止。
 * 返回 0 成功，-1 参数/格式错误（含零包），-2 sink 中止。 */
int vc_ogg_mux(const uint8_t *frames, size_t frames_len, const vc_ogg_opts_t *o, vc_sink_fn sink, void *ctx);
/* 封装后的总字节数，错误返回 0 */
size_t vc_ogg_size(const uint8_t *frames, size_t frames_len, const vc_ogg_opts_t *o);

/* 流式 base64（标准字母表、带 = 填充），写入调用者给的缓冲 */
typedef struct {
    uint8_t carry[3];
    size_t carry_n;
    char *out;
    size_t out_pos;
    size_t out_cap;
    bool overflow;
} vc_b64_t;

static inline size_t vc_b64_len(size_t n) { return ((n + 2) / 3) * 4; }
void vc_b64_init(vc_b64_t *b, char *out, size_t cap);
void vc_b64_update(vc_b64_t *b, const uint8_t *data, size_t len);
/* 返回写出的总字符数，溢出返回 (size_t)-1 */
size_t vc_b64_final(vc_b64_t *b);

/* ---------------- 文本清洗（vc_text.c） ---------------- */

/* UTF-8 安全截断：返回不超过 max 的切点（不会切在多字节字符中间） */
size_t vc_utf8_cut(const char *s, size_t len, size_t max);
/* 校验整段是合法 UTF-8 */
bool vc_utf8_valid(const char *s, size_t len);
/*
 * 把模型/识别文字整理成适合设备字库显示的一行文字：去 Markdown、表情、设备字库外的符号，
 * 合并空白与换行，最后按 UTF-8 截到 max_bytes（含截断时补的“…”）。
 * out_size 至少为 max_bytes + 1。返回写出的字节数。
 */
size_t vc_text_clean(const char *in, char *out, size_t out_size, size_t max_bytes);

/* ---------------- 地址、配置与协议（vc_proto.c） ---------------- */

typedef enum {
    VC_FLAVOR_OPENAI = 0,   /* 普通 OpenAI 兼容：<base>/chat/completions，不加思考参数 */
    VC_FLAVOR_DEEPSEEK,     /* DeepSeek：加 "thinking":{"type":"disabled"} */
    VC_FLAVOR_DASHSCOPE,    /* 百炼兼容模式：<base>/compatible-mode/v1/chat/completions，加 "enable_thinking":false */
} vc_flavor_t;

/* 校验并规范化服务根地址：只接受 https://，不得含用户名/查询/片段/空白；去掉末尾 '/'。
 * strip_suffix=true 时再去掉末尾的 /compatible-mode/v1[/chat/completions] 或 /chat/completions。
 * 返回 0 成功，-1 非法。 */
int vc_base_normalize(const char *in, char *out, size_t out_size, bool strip_suffix);
/* 从 https://host[:port]/... 取 host[:port]；失败返回 -1 */
int vc_url_host(const char *url, char *host, size_t host_size);
int vc_asr_url(const char *asr_base, char *out, size_t out_size);
int vc_chat_url(const char *chat_base, const char *asr_base, const char *model,
                char *out, size_t out_size, vc_flavor_t *flavor);
bool vc_key_valid(const char *key);
bool vc_model_valid(const char *model);

/* ASR 请求体：一次分配（PSRAM），{"model",…,"messages":[system 热词, user input_audio data URI],…}。
 * 返回 NUL 结尾的正文，*out_len 为长度（不含 NUL）；ogg_len 可为 NULL。失败返回 NULL。 */
char *vc_asr_build_body(const uint8_t *frames, size_t frames_len, const char *model,
                        const char *context, size_t *out_len, size_t *ogg_len);
/* 解析 ASR 响应：成功返回 0 并给出 malloc 的文字（已去首尾空白，可能为空串）；格式不对返回 -1 */
int vc_asr_parse(const char *body, size_t len, char **text);

typedef struct {
    const char *user;
    const char *assistant;
} vc_turn_t;

/* 系统提示（now_text 可为 NULL，例如“2026年10月16日 星期五 09:30”） */
char *vc_system_prompt(const char *now_text);
/* 新建 messages 数组：system + 历史 + 本次用户话 */
cJSON *vc_chat_messages(const char *system, const vc_turn_t *hist, size_t hist_n, const char *user);
/* 对话请求体（messages/tools 只引用不接管）；tool_choice 为 NULL 时不带 tools */
char *vc_chat_build_body(const char *model, vc_flavor_t flavor, const cJSON *messages,
                         const cJSON *tools, const char *tool_choice, size_t *out_len);
/* 取 choices[0].message（独立对象，调用者 cJSON_Delete），格式不对返回 NULL */
cJSON *vc_chat_parse_message(const char *body, size_t len);
/* message.content 的文字（字符串或分段数组），返回 malloc 的字符串，没有则返回 "" 的拷贝 */
char *vc_message_text(const cJSON *message);

/* 校验 tools 数组 JSON，返回紧凑格式的拷贝（malloc）；非法返回 NULL */
char *vc_tools_normalize(const char *tools_json);
bool vc_tools_has(const cJSON *tools, const char *name);

/* 错误：给用户看的中文短句（静态字符串）与机器可读的 detail（写入 buf） */
typedef enum { VC_STAGE_ASR = 0, VC_STAGE_CHAT } vc_stage_t;
const char *vc_err_http(vc_stage_t stage, int status, char *detail, size_t detail_size);
const char *vc_err_name(esp_err_t err);

#define VC_MSG_NOT_CONFIGURED "底座模式未配置 AI 服务"
#define VC_MSG_NO_AUDIO       "没有录到声音，请再说一次"
#define VC_MSG_NO_MEM         "内存不足，请稍后再试"
#define VC_MSG_NET            "网络连接失败，请检查 Wi-Fi"
#define VC_MSG_TIMEOUT        "服务响应超时，请再试一次"
#define VC_MSG_BAD_RESPONSE   "服务返回异常，请稍后再试"
#define VC_MSG_ASR_EMPTY      "没听清，请再说一次"
#define VC_MSG_CHAT_EMPTY     "没有得到回复，请再试一次"
#define VC_MSG_TOOLS_DONE     "好的，已经办好了。"

/* ---------------- 会话流水线（vc_pipeline.c） ---------------- */

typedef enum {
    VC_NET_OK = 0,
    VC_NET_FAIL,        /* 连接/收发失败 */
    VC_NET_TIMEOUT,     /* 网络超时 */
    VC_NET_NOMEM,
    VC_NET_TOO_LARGE,   /* 响应超过上限 */
} vc_net_t;

typedef struct {
    int status;         /* HTTP 状态码，传输失败为 0 */
    char *body;         /* vc_big_malloc 分配、NUL 结尾，可能为 NULL；调用者 vc_big_free */
    size_t body_len;
} vc_http_resp_t;

typedef struct {
    /* POST JSON；key 只用于 Authorization 头，实现不得记录。 */
    vc_net_t (*post)(void *ctx, const char *url, const char *key, const char *body, size_t body_len,
                     int timeout_ms, size_t max_resp, vc_http_resp_t *resp);
    void *ctx;
} vc_transport_t;

typedef struct {
    voice_cloud_event_t evt;   /* VOICE_CLOUD_EVT_REPLY 或 VOICE_CLOUD_EVT_ERROR */
    char *text;                /* malloc */
    char detail[40];
} vc_final_t;

typedef struct vc_job {
    /* 输入（全部只读，由调用者保证在 run 期间有效） */
    const char *asr_url, *asr_key, *asr_model;
    const char *chat_url, *chat_key, *chat_model;
    vc_flavor_t flavor;
    const char *asr_context;
    const uint8_t *frames;     /* 为 NULL 时走文字模式 */
    size_t frames_len;
    const char *text;          /* 文字模式的输入 */
    const char *tools_json;    /* 规范化后的 tools 数组，可为 NULL */
    voice_cloud_tool_cb_t tool_cb;
    void *tool_ctx;
    const vc_turn_t *history;
    size_t history_n;
    const char *now_text;
    /* 环境 */
    const vc_transport_t *tp;
    void (*emit)(void *ctx, voice_cloud_event_t evt, const char *text, const char *detail);
    void *emit_ctx;
    bool (*cancelled)(void *ctx);
    void *cancel_ctx;
    int64_t (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, uint32_t ms);   /* 需在取消时尽快返回 */
    void *env_ctx;
    /* ASR 正文建好后回调，调用者可立即释放帧缓存以降低内存峰值（可为 NULL） */
    void (*release_frames)(void *ctx);
    void *release_ctx;
    /* 输出 */
    vc_final_t final;
    char *out_user;            /* 本轮用户文字（malloc），供写历史 */
    char *out_reply;           /* 本轮最终回复（malloc，已清洗），供写历史 */
} vc_job_t;

typedef enum { VC_RUN_OK = 0, VC_RUN_ERROR, VC_RUN_CANCELLED } vc_run_t;

/* 执行一次会话：过程事件（RECOGNIZING/HEARD/THINKING/TOOL）经 emit 发出；
 * 终态（REPLY/ERROR）不发出，而是写进 job->final，由调用者在清除忙状态后发出。 */
vc_run_t vc_pipeline_run(vc_job_t *job);
void vc_job_release(vc_job_t *job);        /* 释放 final.text/out_user/out_reply */

/* 录音时长对应的 ASR 时限 */
uint32_t vc_asr_budget_ms(size_t packets);

/* ---------------- 设备胶水的内部入口（宿主测试/工具用） ---------------- */

/* 由平台提供的 HTTP 传输（设备：vc_http_esp.c；宿主：测试或工具自行实现） */
const vc_transport_t *vc_platform_transport(void);
/* 测试与核验工具用：覆盖热词上下文（NULL 恢复默认，"" 表示不带热词） */
void vc_internal_set_asr_context(const char *context);
/* 测试用：取消一切、等待工作任务退出并释放全部内存，回到未初始化状态 */
void vc_internal_reset(void);
/* 测试用：当前存活的工作任务数 */
int vc_internal_live_workers(void);

#ifdef __cplusplus
}
#endif

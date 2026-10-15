/*
 * 底座模式语音链路（voice_cloud）。
 *
 * 背夹录到的 Opus 包（16 kHz、单声道、60 ms 一包）→ Ogg Opus 封装 → base64 →
 * 设备经 Wi-Fi 直连百炼 qwen3-asr-flash 识别 → OpenAI 兼容对话模型（默认
 * DeepSeek deepseek-flash，关闭思考）+ 少量工具调用 → 通过回调把各阶段状态交给首页。
 *
 * 线程模型（集成前必读，细节见 README.md）：
 * - push_opus 可在任意任务调用，只做一次短临界区内的拷贝，不分配内存、不等待；
 * - finish/ask_text 立即返回，真正的工作在独立工作任务里完成；事件回调与工具回调
 *   都在该工作任务里执行，回调里请只做拷贝/投递，不要长时间阻塞；
 * - 同一时刻只允许一个活动会话；被取消的会话不再回调。
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_CLOUD_EVT_RECOGNIZING = 0, /* 音频已提交，等待识别 */
    VOICE_CLOUD_EVT_HEARD,           /* text = 识别文字 */
    VOICE_CLOUD_EVT_THINKING,        /* 对话请求进行中 */
    VOICE_CLOUD_EVT_TOOL,            /* text = 工具名, detail = 参数 JSON（已执行） */
    VOICE_CLOUD_EVT_REPLY,           /* text = 最终回复（已去 Markdown/表情，适合设备字库） */
    VOICE_CLOUD_EVT_ERROR,           /* text = 给用户看的中文短句, detail = 错误码 */
} voice_cloud_event_t;

/*
 * 事件回调。text/detail 永不为 NULL（没有内容时为 ""），只在回调期间有效，需要保留请自行拷贝。
 * 每个成功启动的会话（finish/ask_text 返回 ESP_OK）恰好以一个 REPLY 或 ERROR 结束，
 * 除非它被 voice_cloud_cancel()/voice_cloud_begin() 取消（取消后不再回调）。
 * 终态回调（REPLY/ERROR）发出时 voice_cloud_busy() 已经为 false，可以在回调里直接开始下一句。
 */
typedef void (*voice_cloud_event_cb_t)(voice_cloud_event_t evt, const char *text, const char *detail, void *user_ctx);

/*
 * 工具回调：name = 工具名，args = 模型给出的参数对象（只读，回调返回后失效），
 * out/out_size = 写给模型看的结果（可写 JSON 或一句话，建议 ≤ 256 字节，必须以 '\0' 结尾）。
 * 返回 ESP_OK 表示执行成功；其他值表示失败，out 里可写失败原因。
 */
typedef esp_err_t (*voice_cloud_tool_cb_t)(const char *name, const cJSON *args, char *out, size_t out_size, void *user_ctx);

esp_err_t voice_cloud_init(void);                 /* 读 NVS 命名空间 "vcloud"；可重复调用 */
bool voice_cloud_asr_ready(void);
bool voice_cloud_chat_ready(void);
esp_err_t voice_cloud_config_set(const cJSON *fields); /* asr_base/asr_key/asr_model/chat_base/chat_key/chat_model，""=清除，持久化 */
cJSON *voice_cloud_config_public(void);           /* 调用者释放；绝不含密钥：asr_ready/chat_ready/asr_model/chat_model/asr_host/chat_host */
esp_err_t voice_cloud_set_tools(const char *tools_json, voice_cloud_tool_cb_t cb, void *user_ctx); /* OpenAI tools 数组 JSON */
esp_err_t voice_cloud_begin(void);                /* 开始收包（取消上一次） */
esp_err_t voice_cloud_push_opus(const uint8_t *pkt, size_t len); /* 任意任务调用；不阻塞；超过 120 s 丢弃 */
esp_err_t voice_cloud_finish(voice_cloud_event_cb_t cb, void *user_ctx); /* 起工作任务：ASR → 对话；立即返回 */
esp_err_t voice_cloud_ask_text(const char *text, voice_cloud_event_cb_t cb, void *user_ctx); /* 跳过 ASR，只走对话 */
void voice_cloud_cancel(void);
bool voice_cloud_busy(void);
size_t voice_cloud_packet_count(void);

/* ---- 以下为新增的辅助接口（可选使用，不影响上面的签名） ---- */

/*
 * 集成契约第 3 节的四个工具（open_app{name}、set_brightness{percent}、set_volume{percent}、
 * set_mode{mode}）的 OpenAI tools 数组 JSON，静态字符串，可直接传给 voice_cloud_set_tools()。
 */
const char *voice_cloud_default_tools_json(void);

/* 清空对话历史（最近 4 轮），例如切换模式或用户说“重新开始”时调用。 */
void voice_cloud_history_clear(void);

#ifdef __cplusplus
}
#endif

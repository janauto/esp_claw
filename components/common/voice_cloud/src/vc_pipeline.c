/*
 * 一次语音会话的流水线：ASR →（听到）→ 对话 +（最多 2 轮）工具调用 → 回复。
 *
 * 与平台无关：HTTP、时钟、休眠、取消、事件输出全部通过 vc_job_t 注入，
 * 因此宿主测试可以用假传输把“工具调用两轮、503 重试、超时、取消”等路径都跑一遍。
 * 日志只记阶段、字节数、耗时与状态码，不记识别文字、回复与密钥。
 */
#include <stdio.h>
#include <string.h>

#include "vc_internal.h"

typedef enum {
    VC_POST_OK = 0,
    VC_POST_HTTP,       /* 拿到了非 2xx 状态码 */
    VC_POST_NET,
    VC_POST_TIMEOUT,
    VC_POST_NOMEM,
    VC_POST_TOO_LARGE,
    VC_POST_CANCELLED,
} vc_post_t;

uint32_t vc_asr_budget_ms(size_t packets)
{
    /* 基础 15 s；长录音按音频时长的 1/8 放宽，最多 30 s（120 s 录音的上传与识别也够用） */
    uint64_t audio_ms = (uint64_t)packets * VC_FRAME_MS;
    uint64_t budget = VC_ASR_BUDGET_MS + audio_ms / 8;
    return budget > VC_ASR_BUDGET_MAX_MS ? VC_ASR_BUDGET_MAX_MS : (uint32_t)budget;
}

static bool is_cancelled(const vc_job_t *j)
{
    return j->cancelled && j->cancelled(j->cancel_ctx);
}

static int64_t now_ms(const vc_job_t *j)
{
    return j->now_ms(j->env_ctx);
}

static void emit(vc_job_t *j, voice_cloud_event_t evt, const char *text, const char *detail)
{
    if (j->emit && !is_cancelled(j)) {
        j->emit(j->emit_ctx, evt, text ? text : "", detail ? detail : "");
    }
}

static vc_run_t fail(vc_job_t *j, const char *text, const char *detail)
{
    free(j->final.text);
    j->final.evt = VOICE_CLOUD_EVT_ERROR;
    j->final.text = vc_strdup(text);
    snprintf(j->final.detail, sizeof(j->final.detail), "%s", detail);
    VC_LOGW("session error: %s", detail); /* detail 是错误码，不含正文 */
    return VC_RUN_ERROR;
}

static void resp_clear(vc_http_resp_t *r)
{
    vc_big_free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->status = 0;
}

/* 发 POST，429/5xx/“too busy”/网络错误按 600 ms、1200 ms 退避重试，最多 2 次，且不超过阶段时限。 */
static vc_post_t post_retry(vc_job_t *j, vc_stage_t stage, const char *url, const char *key, const char *body,
                            size_t len, int64_t deadline, size_t max_resp, vc_http_resp_t *resp)
{
    const char *tag = stage == VC_STAGE_ASR ? "asr" : "chat";
    vc_post_t result = VC_POST_TIMEOUT;
    memset(resp, 0, sizeof(*resp));
    for (int attempt = 0; attempt <= VC_RETRY_MAX; attempt++) {
        if (is_cancelled(j)) {
            return VC_POST_CANCELLED;
        }
        int64_t start = now_ms(j);
        int64_t remain = deadline - start;
        if (remain < VC_HTTP_MIN_MS) {
            break; /* 时间不够再发一次：返回上一次的结果（首轮即超时则为 TIMEOUT） */
        }
        int timeout = remain > VC_HTTP_OP_MAX_MS ? VC_HTTP_OP_MAX_MS : (int)remain;
        resp_clear(resp);
        vc_net_t net = j->tp->post(j->tp->ctx, url, key, body, len, timeout, max_resp, resp);
        int64_t spent = now_ms(j) - start;
        VC_LOGI("%s http: attempt=%d net=%d status=%d req=%u resp=%u bytes %lld ms", tag, attempt + 1, (int)net,
                resp->status, (unsigned)len, (unsigned)resp->body_len, (long long)spent);
        if (is_cancelled(j)) {
            return VC_POST_CANCELLED;
        }
        bool retry;
        if (net == VC_NET_OK) {
            if (resp->status >= 200 && resp->status < 300) {
                return VC_POST_OK;
            }
            result = VC_POST_HTTP;
            retry = resp->status == 429 || resp->status >= 500 || (resp->body && strstr(resp->body, "too busy"));
        } else if (net == VC_NET_TIMEOUT) {
            result = VC_POST_TIMEOUT;
            retry = true;
        } else if (net == VC_NET_NOMEM) {
            result = VC_POST_NOMEM;
            retry = false;
        } else if (net == VC_NET_TOO_LARGE) {
            result = VC_POST_TOO_LARGE;
            retry = false;
        } else {
            result = VC_POST_NET;
            retry = true;
        }
        if (!retry || attempt == VC_RETRY_MAX) {
            break;
        }
        uint32_t backoff = (uint32_t)VC_RETRY_BASE_MS << attempt;
        if (now_ms(j) + backoff + VC_HTTP_MIN_MS > deadline) {
            break;
        }
        j->sleep_ms(j->env_ctx, backoff);
    }
    return result;
}

static vc_run_t fail_post(vc_job_t *j, vc_stage_t stage, vc_post_t r, const vc_http_resp_t *resp)
{
    char detail[40];
    const char *tag = stage == VC_STAGE_ASR ? "asr" : "chat";
    switch (r) {
    case VC_POST_HTTP: {
        const char *text = vc_err_http(stage, resp->status, detail, sizeof(detail));
        return fail(j, text, detail);
    }
    case VC_POST_TIMEOUT:
        snprintf(detail, sizeof(detail), "%s_timeout", tag);
        return fail(j, VC_MSG_TIMEOUT, detail);
    case VC_POST_NOMEM:
        return fail(j, VC_MSG_NO_MEM, "no_mem");
    case VC_POST_TOO_LARGE:
        snprintf(detail, sizeof(detail), "%s_bad_response", tag);
        return fail(j, VC_MSG_BAD_RESPONSE, detail);
    default:
        snprintf(detail, sizeof(detail), "%s_net", tag);
        return fail(j, VC_MSG_NET, detail);
    }
}

/* ---------------- 工具调用 ---------------- */

typedef enum { VC_TOOLS_OK = 0, VC_TOOLS_NOMEM, VC_TOOLS_CANCELLED } vc_tools_rc_t;

/* 模型给的 arguments 一般是 JSON 字符串；个别实现直接给对象。返回新对象，非法返回 NULL。 */
static cJSON *parse_args(const cJSON *raw)
{
    if (cJSON_IsObject(raw)) {
        return cJSON_Duplicate(raw, true);
    }
    if (!raw || cJSON_IsNull(raw)) {
        return cJSON_CreateObject();
    }
    if (!cJSON_IsString(raw)) {
        return NULL;
    }
    const char *s = raw->valuestring;
    while (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') {
        s++;
    }
    if (!*s) {
        return cJSON_CreateObject();
    }
    cJSON *obj = cJSON_Parse(s);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

/* 把助手的 tool_calls 原样追加进 messages，再执行每个调用并追加对应的 tool 消息。 */
static vc_tools_rc_t handle_tool_calls(vc_job_t *j, cJSON *messages, const cJSON *msg, const cJSON *calls,
                                       const cJSON *tools, int *executed)
{
    cJSON *am = cJSON_CreateObject();
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(msg, "reasoning_content");
    cJSON *dup = cJSON_Duplicate(calls, true);
    bool ok = am && dup && cJSON_AddStringToObject(am, "role", "assistant") &&
              cJSON_AddStringToObject(am, "content", cJSON_IsString(content) ? content->valuestring : "");
    /* 开着思考模式时 DeepSeek 要求把本轮推理原样带回；关闭思考时不会出现 */
    if (ok && cJSON_IsString(reasoning)) {
        ok = cJSON_AddStringToObject(am, "reasoning_content", reasoning->valuestring) != NULL;
    }
    if (ok) {
        ok = cJSON_AddItemToObject(am, "tool_calls", dup);
        dup = ok ? NULL : dup;
    }
    if (!ok || !cJSON_AddItemToArray(messages, am)) {
        cJSON_Delete(dup);
        cJSON_Delete(am);
        return VC_TOOLS_NOMEM;
    }

    char *out = malloc(VC_TOOL_OUT_MAX);
    if (!out) {
        return VC_TOOLS_NOMEM;
    }
    vc_tools_rc_t rc = VC_TOOLS_OK;
    int index = 0;
    const cJSON *call;
    cJSON_ArrayForEach(call, calls) {
        if (is_cancelled(j)) {
            rc = VC_TOOLS_CANCELLED;
            break;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
        const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
        const char *tool = cJSON_IsString(name) ? name->valuestring : NULL;
        cJSON *result = cJSON_CreateObject();
        if (!result) {
            rc = VC_TOOLS_NOMEM;
            break;
        }
        if (index >= VC_TOOL_CALLS_PER_ROUND) {
            cJSON_AddFalseToObject(result, "ok");
            cJSON_AddStringToObject(result, "error", "too_many_calls");
        } else if (!tool || !vc_tools_has(tools, tool)) {
            cJSON_AddFalseToObject(result, "ok");
            cJSON_AddStringToObject(result, "error", "unknown_tool");
        } else {
            cJSON *args = parse_args(cJSON_GetObjectItemCaseSensitive(fn, "arguments"));
            if (!args) {
                cJSON_AddFalseToObject(result, "ok");
                cJSON_AddStringToObject(result, "error", "invalid_arguments");
            } else {
                out[0] = '\0';
                esp_err_t err = j->tool_cb(tool, args, out, VC_TOOL_OUT_MAX, j->tool_ctx);
                out[VC_TOOL_OUT_MAX - 1] = '\0';
                if (!vc_utf8_valid(out, strlen(out))) {
                    out[0] = '\0'; /* 非法 UTF-8 会让请求体变成非法 JSON */
                }
                (*executed)++;
                char *args_json = cJSON_PrintUnformatted(args);
                emit(j, VOICE_CLOUD_EVT_TOOL, tool, args_json ? args_json : "{}");
                cJSON_free(args_json);
                VC_LOGI("tool %s: %s", tool, vc_err_name(err));
                if (err == ESP_OK) {
                    cJSON_AddTrueToObject(result, "ok");
                    cJSON *parsed = out[0] ? cJSON_Parse(out) : NULL;
                    if (parsed && (cJSON_IsObject(parsed) || cJSON_IsArray(parsed))) {
                        cJSON_AddItemToObject(result, "result", parsed);
                    } else {
                        cJSON_Delete(parsed);
                        cJSON_AddStringToObject(result, "result", out[0] ? out : "done");
                    }
                } else {
                    cJSON_AddFalseToObject(result, "ok");
                    cJSON_AddStringToObject(result, "error", vc_err_name(err));
                    if (out[0]) {
                        cJSON_AddStringToObject(result, "message", out);
                    }
                }
                cJSON_Delete(args);
            }
        }
        char *result_json = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
        cJSON *tm = cJSON_CreateObject();
        bool added = result_json && tm && cJSON_AddStringToObject(tm, "role", "tool") &&
                     cJSON_AddStringToObject(tm, "tool_call_id", cJSON_IsString(id) ? id->valuestring : "") &&
                     cJSON_AddStringToObject(tm, "content", result_json) && cJSON_AddItemToArray(messages, tm);
        cJSON_free(result_json);
        if (!added) {
            cJSON_Delete(tm);
            rc = VC_TOOLS_NOMEM;
            break;
        }
        index++;
    }
    free(out);
    return rc;
}

/* ---------------- 对话 ---------------- */

static vc_run_t run_chat(vc_job_t *j, const char *user_text)
{
    emit(j, VOICE_CLOUD_EVT_THINKING, "", "");
    int64_t started = now_ms(j);
    int64_t deadline = started + VC_CHAT_BUDGET_MS;
    char *system = vc_system_prompt(j->now_text);
    cJSON *messages = system ? vc_chat_messages(system, j->history, j->history_n, user_text) : NULL;
    free(system);
    if (!messages) {
        return fail(j, VC_MSG_NO_MEM, "no_mem");
    }
    cJSON *tools = (j->tools_json && j->tool_cb) ? cJSON_Parse(j->tools_json) : NULL;
    int executed = 0;
    vc_run_t rc = VC_RUN_ERROR;
    vc_http_resp_t resp = {0};
    for (int round = 1; round <= VC_TOOL_ROUNDS + 1; round++) {
        /* 前 2 轮允许工具；第 3 轮仍带 tools（历史里有工具消息）但 tool_choice=none，逼模型给出文字结论 */
        const char *choice = tools ? (round <= VC_TOOL_ROUNDS ? "auto" : "none") : NULL;
        size_t len = 0;
        char *body = vc_chat_build_body(j->chat_model, j->flavor, messages, tools, choice, &len);
        if (!body) {
            rc = fail(j, VC_MSG_NO_MEM, "no_mem");
            break;
        }
        vc_post_t pr = post_retry(j, VC_STAGE_CHAT, j->chat_url, j->chat_key, body, len, deadline,
                                  VC_CHAT_RESP_MAX, &resp);
        cJSON_free(body);
        if (pr == VC_POST_CANCELLED) {
            rc = VC_RUN_CANCELLED;
            break;
        }
        if (pr != VC_POST_OK) {
            rc = fail_post(j, VC_STAGE_CHAT, pr, &resp);
            break;
        }
        cJSON *msg = vc_chat_parse_message(resp.body, resp.body_len);
        resp_clear(&resp);
        if (!msg) {
            rc = fail(j, VC_MSG_BAD_RESPONSE, "chat_bad_response");
            break;
        }
        const cJSON *calls = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        if (tools && round <= VC_TOOL_ROUNDS && cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
            vc_tools_rc_t tr = handle_tool_calls(j, messages, msg, calls, tools, &executed);
            cJSON_Delete(msg);
            if (tr == VC_TOOLS_CANCELLED) {
                rc = VC_RUN_CANCELLED;
                break;
            }
            if (tr != VC_TOOLS_OK) {
                rc = fail(j, VC_MSG_NO_MEM, "no_mem");
                break;
            }
            continue;
        }
        char *text = vc_message_text(msg);
        cJSON_Delete(msg);
        if (!text) {
            rc = fail(j, VC_MSG_NO_MEM, "no_mem");
            break;
        }
        char reply[VC_REPLY_MAX_BYTES + 1];
        size_t n = vc_text_clean(text, reply, sizeof(reply), VC_REPLY_MAX_BYTES);
        free(text);
        if (n == 0) {
            if (!executed) {
                rc = fail(j, VC_MSG_CHAT_EMPTY, "chat_empty");
                break;
            }
            snprintf(reply, sizeof(reply), "%s", VC_MSG_TOOLS_DONE);
        }
        j->final.evt = VOICE_CLOUD_EVT_REPLY;
        j->final.text = vc_strdup(reply);
        j->out_reply = vc_strdup(reply);
        if (!j->final.text || !j->out_reply) {
            free(j->out_reply);
            j->out_reply = NULL;
            rc = fail(j, VC_MSG_NO_MEM, "no_mem");
            break;
        }
        VC_LOGI("chat: ok rounds=%d tools=%d reply=%u bytes %lld ms", round, executed, (unsigned)n,
                (long long)(now_ms(j) - started));
        rc = VC_RUN_OK;
        break;
    }
    resp_clear(&resp);
    cJSON_Delete(tools);
    cJSON_Delete(messages);
    return rc;
}

/* ---------------- 入口 ---------------- */

vc_run_t vc_pipeline_run(vc_job_t *j)
{
    memset(&j->final, 0, sizeof(j->final));
    j->out_user = NULL;
    j->out_reply = NULL;
    char *user_text = NULL;
    vc_run_t rc;

    if (j->frames) {
        emit(j, VOICE_CLOUD_EVT_RECOGNIZING, "", "");
        int64_t t0 = now_ms(j);
        long packets = vc_frames_count(j->frames, j->frames_len);
        size_t body_len = 0, ogg_len = 0;
        char *body = packets > 0 ? vc_asr_build_body(j->frames, j->frames_len, j->asr_model, j->asr_context,
                                                     &body_len, &ogg_len)
                                 : NULL;
        if (j->release_frames) {
            j->release_frames(j->release_ctx); /* 正文已含全部音频，帧缓存可以先还掉 */
        }
        if (!body) {
            return fail(j, packets > 0 ? VC_MSG_NO_MEM : VC_MSG_NO_AUDIO, packets > 0 ? "no_mem" : "no_audio");
        }
        VC_LOGI("asr: packets=%ld audio=%ld ms ogg=%u body=%u bytes build=%lld ms", packets, packets * VC_FRAME_MS,
                (unsigned)ogg_len, (unsigned)body_len, (long long)(now_ms(j) - t0));
        int64_t deadline = now_ms(j) + vc_asr_budget_ms((size_t)packets);
        vc_http_resp_t resp = {0};
        vc_post_t pr = post_retry(j, VC_STAGE_ASR, j->asr_url, j->asr_key, body, body_len, deadline,
                                  VC_ASR_RESP_MAX, &resp);
        vc_big_free(body);
        if (pr == VC_POST_CANCELLED) {
            resp_clear(&resp);
            return VC_RUN_CANCELLED;
        }
        if (pr != VC_POST_OK) {
            rc = fail_post(j, VC_STAGE_ASR, pr, &resp);
            resp_clear(&resp);
            return rc;
        }
        int pr2 = vc_asr_parse(resp.body, resp.body_len, &user_text);
        resp_clear(&resp);
        if (pr2 != 0) {
            return fail(j, VC_MSG_BAD_RESPONSE, "asr_bad_response");
        }
        if (!user_text[0]) {
            free(user_text);
            return fail(j, VC_MSG_ASR_EMPTY, "asr_empty");
        }
        VC_LOGI("asr: ok text=%u bytes total=%lld ms", (unsigned)strlen(user_text), (long long)(now_ms(j) - t0));
    } else {
        if (!j->text || !j->text[0]) {
            return fail(j, VC_MSG_CHAT_EMPTY, "empty_text");
        }
        user_text = vc_strdup(j->text);
        if (!user_text) {
            return fail(j, VC_MSG_NO_MEM, "no_mem");
        }
    }

    char heard[VC_HEARD_MAX_BYTES + 1];
    vc_text_clean(user_text, heard, sizeof(heard), VC_HEARD_MAX_BYTES);
    emit(j, VOICE_CLOUD_EVT_HEARD, heard, "");
    if (is_cancelled(j)) {
        free(user_text);
        return VC_RUN_CANCELLED;
    }

    if (j->intent_cb) {
        /* 本地指令：设备能确定执行的（调音量/亮度、切模式、打开应用）不交给对话模型 */
        char reply[VC_REPLY_MAX_BYTES + 1];
        reply[0] = '\0';
        if (j->intent_cb(user_text, reply, sizeof(reply), j->intent_ctx) && reply[0]) {
            j->final.evt = VOICE_CLOUD_EVT_REPLY;
            j->final.text = vc_strdup(reply);
            j->out_reply = vc_strdup(reply);
            if (!j->final.text || !j->out_reply) {
                free(j->out_reply);
                j->out_reply = NULL;
                free(user_text);
                return fail(j, VC_MSG_NO_MEM, "no_mem");
            }
            VC_LOGI("intent: handled on device, reply=%u bytes", (unsigned)strlen(reply));
            j->out_user = user_text;
            return VC_RUN_OK;
        }
    }

    rc = run_chat(j, user_text);
    if (rc == VC_RUN_OK) {
        j->out_user = user_text;
    } else {
        free(user_text);
    }
    return rc;
}

void vc_job_release(vc_job_t *j)
{
    free(j->final.text);
    j->final.text = NULL;
    free(j->out_user);
    j->out_user = NULL;
    free(j->out_reply);
    j->out_reply = NULL;
}

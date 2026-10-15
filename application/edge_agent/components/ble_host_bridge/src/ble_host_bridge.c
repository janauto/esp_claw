/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ble_host_bridge.h"
#include "ble_host_apps.h"
#include "voice_frame.h"
#include "esp_timer.h"
#include "system_ui_home.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "cap_lua.h"
#include "system_ui.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_nimble_hci.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "sdkconfig.h"
#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
#include "lua_module_ble_hid.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ble_host_bridge";

#define BLE_HOST_BRIDGE_PROTOCOL_VERSION 1
#define BLE_HOST_BRIDGE_OUTPUT_MAX       4096
#define BLE_HOST_BRIDGE_PATH_MAX         192
#define BLE_HOST_BRIDGE_ADV_MIN_MS       100
#define BLE_HOST_BRIDGE_ADV_MAX_MS       200
#define BLE_HOST_BRIDGE_NOTIFY_RAW_MAX   128
#define BLE_HOST_BRIDGE_CHUNK_DATA_MAX    96
#define BLE_HOST_BRIDGE_NOTIFY_RETRY_COUNT 5
#define BLE_HOST_BRIDGE_PREFERRED_MTU     247
/* Job event pushes share the request queue, so leave room for them alongside
 * host requests. */
#define BLE_HOST_BRIDGE_REQUEST_QUEUE_LEN 6
#define BLE_HOST_BRIDGE_REQUEST_TASK_STACK 8192
#define BLE_HOST_BRIDGE_AUDIO_SAMPLE_RATE  16000
#define BLE_HOST_BRIDGE_AUDIO_CHANNELS     1
#define BLE_HOST_BRIDGE_AUDIO_BITS         16
#define BLE_HOST_BRIDGE_AUDIO_FRAME_MS     60
#define BLE_HOST_BRIDGE_SHORT_NAME_PREFIX  "esp-claw"
#define BLE_HOST_BRIDGE_MAC_SUFFIX_LEN     4
/* Internal op used to hand a cap_lua job event to the request task. Not part of
 * the host-facing protocol; the "__" prefix keeps it out of the advertised ops. */
#define BLE_HOST_BRIDGE_EVENT_OP         "__job_event"
#define BLE_HOST_BRIDGE_EVENT_REQUEST_ID "job-event"
/* Voice uplink: Opus packets go out on their own binary characteristic, session
 * start/stop events on TX with this synthetic id. */
#define BLE_HOST_BRIDGE_VOICE_EVENT_ID     "voice-event"
#define BLE_HOST_BRIDGE_AUDIO_BITRATE      24000
/* ~2.9 s of 60 ms packets: rides out a slow connection interval without making
 * the capture task wait for the radio. */
#define BLE_HOST_BRIDGE_VOICE_QUEUE_LEN    48
#define BLE_HOST_BRIDGE_VOICE_TASK_STACK   4096
#define BLE_HOST_BRIDGE_VOICE_MAX_MS_DEFAULT 30000
#define BLE_HOST_BRIDGE_VOICE_MAX_MS_LIMIT   120000
#define BLE_HOST_BRIDGE_VOICE_BUTTON_MAX_MS  60000
#define BLE_HOST_BRIDGE_VOICE_TAG_LEN      16

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x45, 0x53, 0x50, 0x43, 0x4c, 0x41, 0x57, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0x7d, 0x7b);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x45, 0x53, 0x50, 0x43, 0x4c, 0x41, 0x57, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x10, 0x7d, 0x7b);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x45, 0x53, 0x50, 0x43, 0x4c, 0x41, 0x57, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x10, 0x7d, 0x7b);
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0x45, 0x53, 0x50, 0x43, 0x4c, 0x41, 0x57, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x10, 0x7d, 0x7b);
static const ble_uuid128_t s_audio_uuid = BLE_UUID128_INIT(
    0x45, 0x53, 0x50, 0x43, 0x4c, 0x41, 0x57, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x10, 0x7d, 0x7b);

typedef enum {
    VOICE_ITEM_AUDIO = 0,
    VOICE_ITEM_STARTED,
    VOICE_ITEM_STOPPED,
    VOICE_ITEM_STOP_REQUEST,
} voice_item_kind_t;

/* One entry of the voice queue: an Opus packet, or a session event that has to
 * stay in order with the packets around it. */
typedef struct {
    uint8_t kind;
    uint8_t session;
    uint16_t seq;
    uint32_t frames;
    uint32_t dropped;
    uint32_t gaps;
    char tag[BLE_HOST_BRIDGE_VOICE_TAG_LEN];
    size_t len;
    uint8_t data[];
} voice_item_t;

typedef struct {
    bool active;
    uint8_t session;
    uint8_t session_counter;
    char source[BLE_HOST_BRIDGE_VOICE_TAG_LEN];
    TickType_t started_at;
    uint32_t max_ms;
    uint32_t next_max_ms;
    uint32_t frames;
    uint32_t dropped;
    uint32_t bytes;
} ble_host_bridge_voice_t;

typedef enum {
    BLE_HOST_BRIDGE_IDLE = 0,
    BLE_HOST_BRIDGE_CONNECTED,
    BLE_HOST_BRIDGE_RECEIVING,
    BLE_HOST_BRIDGE_EXECUTING,
} ble_host_bridge_state_t;

typedef struct {
    bool started;
    bool host_synced;
    bool controller_inited;
    bool controller_enabled;
    bool subscribed;
    bool audio_subscribed;
    uint8_t own_addr_type;
    /* Live links across all tenants of this host, not just ours. */
    uint8_t conn_count;
    uint16_t conn_handle;
    uint16_t tx_val_handle;
    uint16_t audio_val_handle;
    uint16_t negotiated_mtu;
    char device_name[32];
    char storage_base_path[BLE_HOST_BRIDGE_PATH_MAX];
    char request_id[48];
    char pending_op[24];
    char last_job_id[CAP_LUA_JOB_ID_LEN];
    uint16_t expected_chunks;
    uint16_t received_chunks;
    size_t request_len;
    char *request_buf;
    esp_err_t startup_err;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t sync_sem;
    QueueHandle_t request_queue;
    TaskHandle_t request_task;
    QueueHandle_t voice_queue;
    TaskHandle_t voice_task;
    ble_host_bridge_voice_t voice;
    ble_host_bridge_state_t state;
} ble_host_bridge_ctx_t;

static ble_host_bridge_ctx_t s_bridge;

static int ble_host_bridge_gap_event(struct ble_gap_event *event, void *arg);
static int ble_host_bridge_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_host_bridge_status_access(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_host_bridge_voice_access(uint16_t conn_handle, uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt, void *arg);
static void handle_voice_start(const char *id, const cJSON *root);
static void handle_voice_stop(const char *id);
static void handle_voice_status(const char *id);
static void bridge_voice_state_cb(bool capturing, const char *tag, void *user_ctx);
static bool voice_enqueue_locked(voice_item_t *item);
static void ble_host_bridge_voice_task(void *arg);
static esp_err_t ble_host_bridge_start_advertising(void);
static void ble_host_bridge_advertise_or_retry(void);
static void handle_hello(const char *id);
static void handle_device_info(const char *id, const char *payload_json);
static void handle_put_lua(const char *id, const char *source);
static void handle_run_lua(const char *id, const char *path, const char *args_json, uint32_t timeout_ms);
static void handle_job_status(const char *id, const char *job_id);
static void handle_job_tail(const char *id, const char *job_id, uint64_t since_seq, uint32_t max_bytes);
static void handle_job_stop(const char *id, const char *job_id);
static void handle_workbuddy_ui(const char *id, const cJSON *root);
static void handle_workbuddy_audio_control(const char *id, const cJSON *root);
static void handle_workbuddy_audio_opus(const char *id, const cJSON *root);
static esp_err_t ble_host_bridge_send_workbuddy_audio(const uint8_t *opus,
                                                      size_t opus_size,
                                                      uint32_t seq,
                                                      void *user_ctx);
static void handle_job_event(const char *job_id, const char *status);
static void bridge_job_event_cb(const cap_lua_job_event_t *event, void *user_ctx);
static void ble_host_bridge_request_task(void *arg);

static esp_err_t make_device_name_with_mac_suffix(const char *base_name, char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    char suffix[BLE_HOST_BRIDGE_MAC_SUFFIX_LEN + 1];
    size_t max_base_len;
    esp_err_t err;
    int written;

    ESP_RETURN_ON_FALSE(base_name && base_name[0] && out && out_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "device name args invalid");
    err = esp_read_mac(mac, ESP_MAC_BT);
    ESP_RETURN_ON_ERROR(err, TAG, "read BT MAC failed");
    ESP_RETURN_ON_FALSE(out_size > BLE_HOST_BRIDGE_MAC_SUFFIX_LEN + 2,
                        ESP_ERR_INVALID_SIZE, TAG, "device name buffer too small");

    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    max_base_len = out_size - BLE_HOST_BRIDGE_MAC_SUFFIX_LEN - 2;
    written = snprintf(out, out_size, "%.*s-%s", (int)max_base_len, base_name, suffix);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size,
                        ESP_ERR_INVALID_SIZE, TAG, "device name with MAC suffix too long");
    return ESP_OK;
}

static const struct ble_gatt_chr_def s_bridge_chrs[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = ble_host_bridge_rx_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_tx_uuid.u,
        .access_cb = ble_host_bridge_status_access,
        .val_handle = &s_bridge.tx_val_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &s_status_uuid.u,
        .access_cb = ble_host_bridge_status_access,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        /* Binary Opus uplink, see voice_frame.h. Reading it returns the voice
         * session status JSON. */
        .uuid = &s_audio_uuid.u,
        .access_cb = ble_host_bridge_voice_access,
        .val_handle = &s_bridge.audio_val_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

static const struct ble_gatt_svc_def s_bridge_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = s_bridge_chrs,
    },
    {0},
};

static const char *state_name(ble_host_bridge_state_t state)
{
    switch (state) {
    case BLE_HOST_BRIDGE_IDLE:
        return "idle";
    case BLE_HOST_BRIDGE_CONNECTED:
        return "connected";
    case BLE_HOST_BRIDGE_RECEIVING:
        return "receiving";
    case BLE_HOST_BRIDGE_EXECUTING:
        return "executing";
    default:
        return "unknown";
    }
}

static size_t response_chunk_data_max(void)
{
    uint16_t mtu = s_bridge.negotiated_mtu ? s_bridge.negotiated_mtu : BLE_HOST_BRIDGE_PREFERRED_MTU;
    size_t limit = mtu > 80 ? (size_t)mtu - 80 : BLE_HOST_BRIDGE_CHUNK_DATA_MAX;

    if (limit < 24) {
        limit = 24;
    }
    if (limit > BLE_HOST_BRIDGE_CHUNK_DATA_MAX) {
        limit = BLE_HOST_BRIDGE_CHUNK_DATA_MAX;
    }
    return limit;
}

static int adv_ms_to_units(int ms)
{
    return (ms * 8) / 5;
}

static void bridge_lock(void)
{
    if (s_bridge.mutex) {
        xSemaphoreTake(s_bridge.mutex, portMAX_DELAY);
    }
}

static void bridge_unlock(void)
{
    if (s_bridge.mutex) {
        xSemaphoreGive(s_bridge.mutex);
    }
}

static void reset_request_locked(void)
{
    s_bridge.request_id[0] = '\0';
    s_bridge.pending_op[0] = '\0';
    s_bridge.expected_chunks = 0;
    s_bridge.received_chunks = 0;
    s_bridge.request_len = 0;
    if (s_bridge.request_buf) {
        s_bridge.request_buf[0] = '\0';
    }
    if (s_bridge.state == BLE_HOST_BRIDGE_RECEIVING) {
        s_bridge.state = s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE ?
                         BLE_HOST_BRIDGE_CONNECTED : BLE_HOST_BRIDGE_IDLE;
    }
}

static char *make_response(const char *id, bool ok, const char *type, const char *data)
{
    cJSON *root = cJSON_CreateObject();
    char *out;

    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "id", id ? id : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "type", type ? type : "result");
    if (data) {
        cJSON_AddStringToObject(root, ok ? "data" : "error", data);
    }
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static esp_err_t notify_packet(const char *text)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int attempt = 0; attempt < BLE_HOST_BRIDGE_NOTIFY_RETRY_COUNT; attempt++) {
        bridge_lock();
        if (!s_bridge.subscribed || s_bridge.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            bridge_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        uint16_t conn_handle = s_bridge.conn_handle;
        uint16_t tx_handle = s_bridge.tx_val_handle;
        bridge_unlock();

        struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
        if (!om) {
            return ESP_ERR_NO_MEM;
        }
        int rc = ble_gatts_notify_custom(conn_handle, tx_handle, om);
        if (rc == 0) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "notify failed rc=%d attempt=%d", rc, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return ESP_FAIL;
}

/* 按 UTF-8 字符边界切片：手机逐片解码，中文被拆成两半会显示成乱码（如“跳��游戏”） */
static size_t utf8_chunk_len(const char *text, size_t off, size_t len, size_t max)
{
    size_t n = len - off;

    if (n <= max) {
        return n;
    }
    n = max;
    while (n > 0 && ((unsigned char)text[off + n] & 0xC0) == 0x80) {
        n--;
    }
    return n ? n : max;
}

static esp_err_t notify_response_text(const char *id, const char *text)
{
    size_t len;
    const size_t chunk_size = response_chunk_data_max();

    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    len = strlen(text);
    if (len <= BLE_HOST_BRIDGE_NOTIFY_RAW_MAX) {
        return notify_packet(text);
    }

    size_t total = 0;
    for (size_t pos = 0; pos < len; pos += utf8_chunk_len(text, pos, len, chunk_size)) {
        total++;
    }
    size_t off = 0;
    for (size_t seq = 0; seq < total; seq++) {
        size_t chunk = utf8_chunk_len(text, off, len, chunk_size);
        char *part = calloc(1, chunk + 1);
        cJSON *frame = cJSON_CreateObject();
        char *frame_text = NULL;
        esp_err_t err = ESP_OK;

        if (!part || !frame) {
            free(part);
            cJSON_Delete(frame);
            return ESP_ERR_NO_MEM;
        }
        memcpy(part, text + off, chunk);
        cJSON_AddStringToObject(frame, "i", id ? id : "");
        cJSON_AddStringToObject(frame, "k", "c");
        cJSON_AddNumberToObject(frame, "s", (double)seq);
        cJSON_AddNumberToObject(frame, "t", (double)total);
        cJSON_AddStringToObject(frame, "d", part);
        frame_text = cJSON_PrintUnformatted(frame);
        if (!frame_text) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = notify_packet(frame_text);
        }
        free(frame_text);
        cJSON_Delete(frame);
        free(part);
        if (err != ESP_OK) {
            return err;
        }
        off += chunk;
        if (seq + 1 < total) {
            vTaskDelay(pdMS_TO_TICKS(8));
        }
    }
    return ESP_OK;
}

static void send_response(const char *id, bool ok, const char *type, const char *data)
{
    char *response = make_response(id, ok, type, data);

    if (!response) {
        return;
    }
    if (notify_response_text(id, response) != ESP_OK) {
        ESP_LOGI(TAG, "BLE response: %s", response);
    }
    free(response);
}

static esp_err_t ensure_host_dir(char *out_dir, size_t out_dir_size)
{
    int written;

    ESP_RETURN_ON_FALSE(out_dir && out_dir_size > 0, ESP_ERR_INVALID_ARG, TAG, "dir buffer is invalid");
    written = snprintf(out_dir, out_dir_size, "%s/scripts", s_bridge.storage_base_path);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_dir_size, ESP_ERR_INVALID_SIZE, TAG,
                        "scripts path too long");
    (void)mkdir(out_dir, 0775);

    written = snprintf(out_dir, out_dir_size, "%s/scripts/host", s_bridge.storage_base_path);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_dir_size, ESP_ERR_INVALID_SIZE, TAG,
                        "host scripts path too long");
    (void)mkdir(out_dir, 0775);
    return ESP_OK;
}

static esp_err_t save_lua_script(const char *id, const char *source, char *out_path, size_t out_path_size)
{
    char dir[BLE_HOST_BRIDGE_PATH_MAX];
    FILE *fp;
    int written;

    ESP_RETURN_ON_FALSE(id && id[0] && source && out_path && out_path_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "script save args invalid");
    ESP_RETURN_ON_ERROR(ensure_host_dir(dir, sizeof(dir)), TAG, "host dir unavailable");
    written = snprintf(out_path, out_path_size, "%s/%s.lua", dir, id);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_path_size, ESP_ERR_INVALID_SIZE, TAG,
                        "script path too long");

    fp = fopen(out_path, "wb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "failed to open %s", out_path);
    size_t len = strlen(source);
    size_t wrote = fwrite(source, 1, len, fp);
    fclose(fp);
    ESP_RETURN_ON_FALSE(wrote == len, ESP_FAIL, TAG, "failed to write %s", out_path);
    return ESP_OK;
}

static void handle_put_lua(const char *id, const char *source)
{
    char path[BLE_HOST_BRIDGE_PATH_MAX];
    esp_err_t err = save_lua_script(id, source, path, sizeof(path));

    send_response(id, err == ESP_OK, "put_lua", err == ESP_OK ? path : esp_err_to_name(err));
}

static void handle_hello(const char *id)
{
    char data[1800];
    snprintf(data, sizeof(data),
             "{\"protocol\":%d,\"release\":\"" SYSTEM_UI_RELEASE "\",\"mode\":\"%s\","
             "\"device_name\":\"%s\",\"ops\":[%s,\"hello\",\"device_info\",\"put_lua\","
             "\"app_list\",\"app_info\",\"app_read\",\"app_begin\",\"app_write\",\"app_commit\",\"app_abort\",\"app_remove\",\"app_reload\",\"app_rename\","
             "\"run_lua\",\"job_status\",\"job_tail\",\"job_stop\",\"workbuddy_ui\","
             "\"workbuddy_audio_control\",\"workbuddy_audio_opus\",\"voice_start\",\"voice_stop\","
             "\"voice_status\"],"
             "\"pushes\":[\"job_event\",\"workbuddy_audio_opus\",\"voice_event\"],"
             "\"voice\":{\"version\":%d,\"char\":\"7b7d1005-0000-0000-0057-414c43505345\","
             "\"codec\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_ms\":%d,"
             "\"bitrate\":%d,\"header\":%d}}",
             BLE_HOST_BRIDGE_PROTOCOL_VERSION, system_ui_home_mode_name(), s_bridge.device_name,
             system_ui_home_ops_json(), VOICE_FRAME_VERSION,
             BLE_HOST_BRIDGE_AUDIO_SAMPLE_RATE, BLE_HOST_BRIDGE_AUDIO_CHANNELS,
             BLE_HOST_BRIDGE_AUDIO_FRAME_MS, BLE_HOST_BRIDGE_AUDIO_BITRATE, VOICE_FRAME_HEADER_LEN);
    send_response(id, true, "hello", data);
}

static void handle_device_info(const char *id, const char *payload_json)
{
    char *output = calloc(1, BLE_HOST_BRIDGE_OUTPUT_MAX);
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .channel = "ble_host",
        .source_cap = "ble_host_bridge",
    };
    const char *input = "{\"sections\":[\"chip\",\"version\",\"memory\",\"wifi\",\"ip\"]}";

    ESP_LOGI(TAG, "device_info request id=%s", id ? id : "");
    if (!output) {
        send_response(id, false, "error", "out_of_memory");
        return;
    }
    if (payload_json && payload_json[0]) {
        cJSON *payload = cJSON_Parse(payload_json);
        cJSON *sections = payload ? cJSON_GetObjectItemCaseSensitive(payload, "sections") : NULL;

        if (cJSON_IsObject(payload) && cJSON_IsArray(sections) && cJSON_GetArraySize(sections) > 0) {
            input = payload_json;
        } else {
            ESP_LOGW(TAG, "device_info payload missing valid sections, using default selection");
        }
        cJSON_Delete(payload);
    }

    esp_err_t err = claw_cap_call("get_system_info", input, &ctx, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
    send_response(id, err == ESP_OK, "device_info", err == ESP_OK ? output : esp_err_to_name(err));
    free(output);
}

static char *bridge_allocate_output_buffer(void)
{
    return calloc(1, BLE_HOST_BRIDGE_OUTPUT_MAX);
}

/* cap_lua reports progress as human-readable text, not JSON, so the job id has
 * to be scraped out of it. Handles both "Started Lua job <id> (...)" and the
 * "Lua job <id> (...) ended early ..." early-failure form. */
static bool bridge_extract_job_id(const char *text, char *out_id, size_t out_size)
{
    static const char *const markers[] = {"Started Lua job ", "Lua job ", "job_id="};

    if (!text || !out_id || out_size == 0) {
        return false;
    }
    out_id[0] = '\0';

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        const char *hit = strstr(text, markers[i]);
        if (!hit) {
            continue;
        }
        hit += strlen(markers[i]);
        size_t len = 0;
        while (hit[len] && len < out_size - 1 &&
                ((hit[len] >= '0' && hit[len] <= '9') ||
                 (hit[len] >= 'a' && hit[len] <= 'z') ||
                 (hit[len] >= 'A' && hit[len] <= 'Z'))) {
            len++;
        }
        if (len > 0) {
            memcpy(out_id, hit, len);
            out_id[len] = '\0';
            return true;
        }
    }
    return false;
}

static void handle_run_lua(const char *id, const char *path, const char *args_json, uint32_t timeout_ms)
{
    char *output = bridge_allocate_output_buffer();
    esp_err_t err;

    if (!output) {
        send_response(id, false, "error", "out_of_memory");
        return;
    }
    if (!path || !path[0] || strstr(path, "/scripts/host/") == NULL) {
        free(output);
        send_response(id, false, "error", "path_must_be_under_scripts_host");
        return;
    }

    bridge_lock();
    s_bridge.state = BLE_HOST_BRIDGE_EXECUTING;
    bridge_unlock();

    /* timeout_ms 直接透传：0 在 cap_lua 异步路径里表示"跑到被取消为止"，
     * 游戏/常驻界面就靠这个值撑住。缺字段时的 60s 默认在调用处补，
     * 这里再补一次会把显式的 0 吃掉，让长跑脚本 60s 就被杀。 */
    err = cap_lua_run_script_async(path,
                                   args_json && args_json[0] ? args_json : "{}",
                                   timeout_ms,
                                   id && id[0] ? id : "ble-host",
                                   "ble_host",
                                   true,
                                   output,
                                   BLE_HOST_BRIDGE_OUTPUT_MAX);

    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    bool have_job_id = err == ESP_OK && bridge_extract_job_id(output, job_id, sizeof(job_id));

    /* Always leave EXECUTING, including on the error path, or the bridge state
     * stays stuck at "executing" for every later status read. */
    bridge_lock();
    if (have_job_id) {
        strlcpy(s_bridge.last_job_id, job_id, sizeof(s_bridge.last_job_id));
    }
    s_bridge.state = s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE ?
                     BLE_HOST_BRIDGE_CONNECTED : BLE_HOST_BRIDGE_IDLE;
    bridge_unlock();

    if (err == ESP_OK && !have_job_id) {
        ESP_LOGW(TAG, "run_lua: could not parse job id from cap_lua output");
    }
    send_response(id, err == ESP_OK, "run_lua", err == ESP_OK ? output : esp_err_to_name(err));
    free(output);
}

static void handle_job_status(const char *id, const char *job_id)
{
    char *output = bridge_allocate_output_buffer();
    char fallback_id[CAP_LUA_JOB_ID_LEN] = {0};
    esp_err_t err;

    ESP_LOGI(TAG, "job_status request id=%s job_id=%s", id ? id : "", job_id ? job_id : "");
    if (!output) {
        send_response(id, false, "error", "out_of_memory");
        return;
    }
    if (!job_id || !job_id[0]) {
        bridge_lock();
        strlcpy(fallback_id, s_bridge.last_job_id, sizeof(fallback_id));
        bridge_unlock();
        job_id = fallback_id;
    }
    if (!job_id || !job_id[0]) {
        free(output);
        send_response(id, false, "error", "job_id_required");
        return;
    }
    err = cap_lua_get_job(job_id, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
    send_response(id, err == ESP_OK, "job_status", err == ESP_OK ? output : esp_err_to_name(err));
    free(output);
}

/* job_status only carries a bounded log tail. For long scripts the host needs to
 * pull the log incrementally, so expose cap_lua's tail capability as its own op. */
static void handle_job_tail(const char *id, const char *job_id, uint64_t since_seq, uint32_t max_bytes)
{
    char *output = bridge_allocate_output_buffer();
    char fallback_id[CAP_LUA_JOB_ID_LEN] = {0};
    char input[128];
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .channel = "ble_host",
        .source_cap = "ble_host_bridge",
    };

    if (!output) {
        send_response(id, false, "error", "out_of_memory");
        return;
    }
    if (!job_id || !job_id[0]) {
        bridge_lock();
        strlcpy(fallback_id, s_bridge.last_job_id, sizeof(fallback_id));
        bridge_unlock();
        job_id = fallback_id;
    }
    if (!job_id[0]) {
        free(output);
        send_response(id, false, "error", "job_id_required");
        return;
    }

    snprintf(input, sizeof(input),
             "{\"job_id\":\"%.16s\",\"since_seq\":%llu,\"max_bytes\":%u}",
             job_id,
             (unsigned long long)since_seq,
             (unsigned)(max_bytes ? max_bytes : 1024));
    esp_err_t err = claw_cap_call("lua_tail_async_job", input, &ctx, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
    send_response(id, err == ESP_OK, "job_tail", err == ESP_OK ? output : esp_err_to_name(err));
    free(output);
}

static void handle_job_stop(const char *id, const char *job_id)
{
    char *output = bridge_allocate_output_buffer();
    char fallback_id[CAP_LUA_JOB_ID_LEN] = {0};
    esp_err_t err;

    ESP_LOGI(TAG, "job_stop request id=%s job_id=%s", id ? id : "", job_id ? job_id : "");
    if (!output) {
        send_response(id, false, "error", "out_of_memory");
        return;
    }
    if (!job_id || !job_id[0]) {
        bridge_lock();
        strlcpy(fallback_id, s_bridge.last_job_id, sizeof(fallback_id));
        bridge_unlock();
        job_id = fallback_id;
    }
    if (!job_id || !job_id[0]) {
        free(output);
        send_response(id, false, "error", "job_id_required");
        return;
    }
    err = cap_lua_stop_job(job_id, 1500, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
    send_response(id, err == ESP_OK, "job_stop", output[0] ? output : esp_err_to_name(err));
    free(output);
}

static const cJSON *workbuddy_payload_field(const cJSON *root, const char *name)
{
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *fields = payload ? cJSON_GetObjectItemCaseSensitive(payload, "fields") : NULL;
    const cJSON *item = NULL;

    if (fields && cJSON_IsObject(fields)) {
        item = cJSON_GetObjectItemCaseSensitive(fields, name);
    }
    if (!item && payload && cJSON_IsObject(payload)) {
        item = cJSON_GetObjectItemCaseSensitive(payload, name);
    }
    if (!item) {
        item = cJSON_GetObjectItemCaseSensitive(root, name);
    }
    return item;
}

static const char *workbuddy_json_value(const cJSON *item, char *buf, size_t buf_size)
{
    if (cJSON_IsString(item)) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item) && buf && buf_size > 0) {
        snprintf(buf, buf_size, "%d", item->valueint);
        return buf;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? "1" : "0";
    }
    return NULL;
}

static void workbuddy_apply_frame(system_ui_workbuddy_update_t *update,
                                  const char *frame,
                                  char *reply_status,
                                  size_t reply_status_size,
                                  char *reply_text,
                                  size_t reply_text_size,
                                  char *workbuddy_status,
                                  size_t workbuddy_status_size)
{
    const char *sep = frame ? strchr(frame, '=') : NULL;
    size_t key_len;
    const char *value;

    if (!sep || sep == frame) {
        return;
    }
    key_len = (size_t)(sep - frame);
    value = sep + 1;
    if (key_len == strlen("online") && strncmp(frame, "online", key_len) == 0) {
        update->online = value;
    } else if (key_len == strlen("tasks") && strncmp(frame, "tasks", key_len) == 0) {
        update->tasks = value;
    } else if (key_len == strlen("credits") && strncmp(frame, "credits", key_len) == 0) {
        update->credits = value;
    } else if (key_len == strlen("battery") && strncmp(frame, "battery", key_len) == 0) {
        update->battery = value;
    } else if (key_len == strlen("record") && strncmp(frame, "record", key_len) == 0) {
        update->record = value;
    } else if (key_len == strlen("reply_status") && strncmp(frame, "reply_status", key_len) == 0) {
        strlcpy(reply_status, value, reply_status_size);
        update->reply_status = reply_status;
    } else if (key_len == strlen("reply_text") && strncmp(frame, "reply_text", key_len) == 0) {
        strlcpy(reply_text, value, reply_text_size);
        update->reply_text = reply_text;
    } else if (key_len == strlen("reply") && strncmp(frame, "reply", key_len) == 0) {
        strlcpy(reply_text, value, reply_text_size);
        update->reply_text = reply_text;
    } else if (key_len == strlen("workbuddy_status") && strncmp(frame, "workbuddy_status", key_len) == 0) {
        strlcpy(workbuddy_status, value, workbuddy_status_size);
        update->workbuddy_status = workbuddy_status;
    }
}

static void handle_workbuddy_ui(const char *id, const cJSON *root)
{
    char online[16];
    char tasks[16];
    char credits[16];
    char battery[16];
    char record[16];
    char reply_status[48];
    char reply_text[256];
    char workbuddy_status[64];
    system_ui_workbuddy_update_t update = {0};
    const char *names[] = {"online", "tasks", "credits", "battery", "record",
                           "reply_status", "reply_text", "workbuddy_status"};
    char *buffers[] = {online, tasks, credits, battery, record, reply_status, reply_text, workbuddy_status};
    const size_t sizes[] = {sizeof(online), sizeof(tasks), sizeof(credits), sizeof(battery), sizeof(record),
                            sizeof(reply_status), sizeof(reply_text), sizeof(workbuddy_status)};
    const char **targets[] = {&update.online, &update.tasks, &update.credits, &update.battery, &update.record,
                              &update.reply_status, &update.reply_text, &update.workbuddy_status};
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *frames = payload ? cJSON_GetObjectItemCaseSensitive(payload, "frames") : NULL;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const cJSON *item = workbuddy_payload_field(root, names[i]);
        const char *value = workbuddy_json_value(item, buffers[i], sizes[i]);
        if (value) {
            *targets[i] = value;
        }
    }

    if (cJSON_IsArray(frames)) {
        const cJSON *frame = NULL;
        cJSON_ArrayForEach(frame, frames) {
            if (cJSON_IsString(frame)) {
                workbuddy_apply_frame(&update,
                                      frame->valuestring,
                                      reply_status,
                                      sizeof(reply_status),
                                      reply_text,
                                      sizeof(reply_text),
                                      workbuddy_status,
                                      sizeof(workbuddy_status));
            }
        }
    }

    esp_err_t err = system_ui_update_workbuddy(&update);
    /* 0.6.0 App 用 workbuddy_ui 推“识别中 / 你说 / 回复”：同步到首页语音卡片 */
    system_ui_home_note_workbuddy(update.workbuddy_status, update.reply_status, update.reply_text);
    send_response(id, err == ESP_OK, "workbuddy_ui", err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static const char *workbuddy_audio_payload_string(const cJSON *root, const char *name)
{
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *item = payload && cJSON_IsObject(payload) ?
                        cJSON_GetObjectItemCaseSensitive(payload, name) : NULL;

    if (!item) {
        item = cJSON_GetObjectItemCaseSensitive(root, name);
    }
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void handle_workbuddy_audio_control(const char *id, const cJSON *root)
{
    const char *action = workbuddy_audio_payload_string(root, "action");
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (action && strcmp(action, "start_capture") == 0) {
        err = system_ui_workbuddy_start_audio_session(true);
    } else if (action && strcmp(action, "start_playback") == 0) {
        err = system_ui_workbuddy_start_audio_session(false);
    } else if (action && strcmp(action, "stop") == 0) {
        err = system_ui_workbuddy_stop_audio_session();
    }
    send_response(id, err == ESP_OK, "workbuddy_audio_control",
                  err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void handle_workbuddy_audio_opus(const char *id, const cJSON *root)
{
    const char *b64 = workbuddy_audio_payload_string(root, "data");
    size_t b64_len = b64 ? strlen(b64) : 0;
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    esp_err_t err = ESP_OK;

    if (!b64 || b64_len == 0) {
        send_response(id, false, "workbuddy_audio_opus", "missing_data");
        return;
    }

    decoded = malloc(b64_len);
    if (!decoded) {
        send_response(id, false, "workbuddy_audio_opus", "out_of_memory");
        return;
    }
    if (mbedtls_base64_decode(decoded, b64_len, &decoded_len,
                              (const unsigned char *)b64, b64_len) != 0 ||
            decoded_len == 0) {
        free(decoded);
        send_response(id, false, "workbuddy_audio_opus", "base64_decode_failed");
        return;
    }

    err = system_ui_workbuddy_play_opus(decoded, decoded_len);
    free(decoded);
    if (err != ESP_OK) {
        send_response(id, false, "workbuddy_audio_opus", esp_err_to_name(err));
    }
}

static esp_err_t ble_host_bridge_send_workbuddy_audio(const uint8_t *opus,
                                                      size_t opus_size,
                                                      uint32_t seq,
                                                      void *user_ctx)
{
    (void)user_ctx;

    if (!opus || opus_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bridge_lock();
    bool deliverable = s_bridge.subscribed && s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE;
    bool binary = s_bridge.audio_subscribed && s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                  s_bridge.voice_queue != NULL;
    bridge_unlock();

    if (binary) {
        /* Runs on the capture task: hand the packet to the voice task and return,
         * so the radio never makes the mic reader miss a chunk. */
        voice_item_t *item = malloc(sizeof(*item) + opus_size);
        bool queued = false;

        if (item) {
            memset(item, 0, sizeof(*item));
            item->kind = VOICE_ITEM_AUDIO;
            item->seq = (uint16_t)seq;
            item->len = opus_size;
            memcpy(item->data, opus, opus_size);
        }
        bridge_lock();
        /* Packets outside a session (encoder tail after stop) are discarded so
         * the host never sees audio after the "stopped" event. */
        bool active = s_bridge.voice.active;
        if (item && active) {
            item->session = s_bridge.voice.session;
            queued = voice_enqueue_locked(item);
            if (queued) {
                s_bridge.voice.frames++;
                s_bridge.voice.bytes += (uint32_t)opus_size;
            } else {
                s_bridge.voice.dropped++;
            }
        } else if (active) {
            s_bridge.voice.dropped++;
        }
        bridge_unlock();
        if (!queued) {
            free(item);
        }
        return queued || !active ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!deliverable) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t b64_capacity = ((opus_size + 2) / 3) * 4 + 1;
    char *b64 = calloc(1, b64_capacity);
    cJSON *root = cJSON_CreateObject();
    char *text = NULL;
    size_t b64_len = 0;
    esp_err_t err = ESP_OK;

    if (!b64 || !root) {
        free(b64);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (mbedtls_base64_encode((unsigned char *)b64, b64_capacity, &b64_len, opus, opus_size) != 0) {
        free(b64);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "id", BLE_HOST_BRIDGE_EVENT_REQUEST_ID);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "workbuddy_audio_opus");
    cJSON_AddNumberToObject(root, "seq", (double)seq);
    cJSON_AddNumberToObject(root, "sample_rate", BLE_HOST_BRIDGE_AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "channels", BLE_HOST_BRIDGE_AUDIO_CHANNELS);
    cJSON_AddNumberToObject(root, "bits", BLE_HOST_BRIDGE_AUDIO_BITS);
    cJSON_AddNumberToObject(root, "frame_ms", BLE_HOST_BRIDGE_AUDIO_FRAME_MS);
    cJSON_AddStringToObject(root, "data", b64);
    text = cJSON_PrintUnformatted(root);
    if (!text) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = notify_response_text(BLE_HOST_BRIDGE_EVENT_REQUEST_ID, text);
    }
    free(text);
    cJSON_Delete(root);
    free(b64);
    return err;
}

/* ---- Voice uplink -------------------------------------------------------- */

/* Caller holds the bridge lock. Never blocks: the capture task and LVGL event
 * handlers both end up here. */
static bool voice_enqueue_locked(voice_item_t *item)
{
    if (!item || !s_bridge.voice_queue) {
        return false;
    }
    return xQueueSend(s_bridge.voice_queue, &item, 0) == pdTRUE;
}

static voice_item_t *voice_item_new(voice_item_kind_t kind, const char *tag)
{
    voice_item_t *item = calloc(1, sizeof(*item));

    if (item) {
        item->kind = (uint8_t)kind;
        strlcpy(item->tag, tag ? tag : "", sizeof(item->tag));
    }
    return item;
}

static esp_err_t notify_audio(const uint8_t *data, size_t len)
{
    for (int attempt = 0; attempt < BLE_HOST_BRIDGE_NOTIFY_RETRY_COUNT; attempt++) {
        bridge_lock();
        if (!s_bridge.audio_subscribed || s_bridge.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            bridge_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        uint16_t conn_handle = s_bridge.conn_handle;
        uint16_t audio_handle = s_bridge.audio_val_handle;
        bridge_unlock();

        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (!om) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }
        int rc = ble_gatts_notify_custom(conn_handle, audio_handle, om);
        if (rc == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return ESP_FAIL;
}

static void voice_send_packet(const voice_item_t *item)
{
    uint8_t frag[BLE_HOST_BRIDGE_PREFERRED_MTU];
    voice_frame_cursor_t cursor;
    size_t cap;
    size_t n;

    bridge_lock();
    uint16_t mtu = s_bridge.negotiated_mtu ? s_bridge.negotiated_mtu : 23;
    bridge_unlock();
    /* ATT notification payload is MTU - 3. */
    cap = mtu > 3 ? (size_t)mtu - 3 : 20;
    if (cap > sizeof(frag)) {
        cap = sizeof(frag);
    }

    voice_frame_cursor_init(&cursor, item->session, item->seq, item->data, item->len);
    while ((n = voice_frame_next(&cursor, frag, cap)) > 0) {
        if (notify_audio(frag, n) != ESP_OK) {
            bridge_lock();
            s_bridge.voice.dropped++;
            bridge_unlock();
            return;
        }
    }
}

static void voice_status_json(char *out, size_t out_size)
{
    bridge_lock();
    uint32_t elapsed_ms = s_bridge.voice.active ?
                          (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_bridge.voice.started_at) : 0;
    snprintf(out, out_size,
             "{\"active\":%s,\"session\":%u,\"source\":\"%s\",\"frames\":%" PRIu32
             ",\"dropped\":%" PRIu32 ",\"bytes\":%" PRIu32 ",\"elapsed_ms\":%" PRIu32
             ",\"max_ms\":%" PRIu32 ",\"mic_gaps\":%" PRIu32 ",\"audio_subscribed\":%s,"
             "\"codec\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_ms\":%d,\"bitrate\":%d}",
             s_bridge.voice.active ? "true" : "false",
             (unsigned)s_bridge.voice.session,
             s_bridge.voice.source,
             s_bridge.voice.frames,
             s_bridge.voice.dropped,
             s_bridge.voice.bytes,
             elapsed_ms,
             s_bridge.voice.max_ms,
             system_ui_workbuddy_audio_mic_gaps(),
             s_bridge.audio_subscribed ? "true" : "false",
             BLE_HOST_BRIDGE_AUDIO_SAMPLE_RATE,
             BLE_HOST_BRIDGE_AUDIO_CHANNELS,
             BLE_HOST_BRIDGE_AUDIO_FRAME_MS,
             BLE_HOST_BRIDGE_AUDIO_BITRATE);
    bridge_unlock();
}

static void voice_send_event(const voice_item_t *item)
{
    cJSON *root = cJSON_CreateObject();
    char *text;

    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "id", BLE_HOST_BRIDGE_VOICE_EVENT_ID);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "voice_event");
    cJSON_AddStringToObject(root, "state", item->kind == VOICE_ITEM_STARTED ? "started" : "stopped");
    cJSON_AddNumberToObject(root, "session", item->session);
    if (item->kind == VOICE_ITEM_STARTED) {
        cJSON_AddStringToObject(root, "source", item->tag);
        /* 1.0：设备状态给手机对话用（天气可直接回答，不必打开应用） */
        const char *ctx = system_ui_home_voice_context();
        if (ctx && ctx[0]) {
            cJSON_AddStringToObject(root, "context", ctx);
        }
    } else {
        cJSON_AddStringToObject(root, "reason", item->tag);
        cJSON_AddNumberToObject(root, "frames", (double)item->frames);
        cJSON_AddNumberToObject(root, "dropped", (double)item->dropped);
        cJSON_AddNumberToObject(root, "gaps", (double)item->gaps);
    }
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) {
        if (notify_response_text(BLE_HOST_BRIDGE_VOICE_EVENT_ID, text) != ESP_OK) {
            ESP_LOGW(TAG, "voice event not delivered: %s", text);
        }
        free(text);
    }
}

/* Runs on the voice task or the request task, never in a NimBLE or LVGL
 * callback: stopping touches the filesystem and the UI lock. */
static void voice_stop_capture(const char *reason)
{
    if (!system_ui_workbuddy_audio_capturing()) {
        return;
    }
    (void)system_ui_workbuddy_stop_audio_session_from(reason);
    (void)system_ui_update_workbuddy(&(system_ui_workbuddy_update_t) {
        .record = "0",
        .workbuddy_status = strcmp(reason, "timeout") == 0 ? "录音已到上限" :
                            strcmp(reason, "host") == 0 ? "识别中" : "等待中",
    });
    ESP_LOGI(TAG, "voice capture stopped reason=%s", reason);
}

/* system_ui reports every capture start/stop here, whoever caused it (record
 * button, meeting screen, host op). May run inside an LVGL event handler, so
 * only record state and queue the event. */
static void bridge_voice_state_cb(bool capturing, const char *tag, void *user_ctx)
{
    /* 1.0 底座模式：首页的录音交给本机 voice_cloud，不通知手机。 */
    if (capturing && tag && strcmp(tag, "home-dock") == 0) {
        return;
    }
    voice_item_t *item = voice_item_new(capturing ? VOICE_ITEM_STARTED : VOICE_ITEM_STOPPED, tag);
    (void)user_ctx;

    bridge_lock();
    if (capturing) {
        if (s_bridge.voice.active) {
            bridge_unlock();
            free(item);
            return;
        }
        s_bridge.voice.session_counter++;
        if (s_bridge.voice.session_counter == 0) {
            s_bridge.voice.session_counter = 1;
        }
        s_bridge.voice.active = true;
        s_bridge.voice.session = s_bridge.voice.session_counter;
        strlcpy(s_bridge.voice.source, tag ? tag : "device", sizeof(s_bridge.voice.source));
        s_bridge.voice.started_at = xTaskGetTickCount();
        /* Cap utterances (host op, record button) so a forgotten session does
         * not stream forever; meeting recordings stay uncapped. */
        if (s_bridge.voice.next_max_ms) {
            s_bridge.voice.max_ms = s_bridge.voice.next_max_ms;
        } else if (tag && strcmp(tag, "meeting") == 0) {
            s_bridge.voice.max_ms = 0;
        } else {
            s_bridge.voice.max_ms = BLE_HOST_BRIDGE_VOICE_BUTTON_MAX_MS;
        }
        s_bridge.voice.next_max_ms = 0;
        s_bridge.voice.frames = 0;
        s_bridge.voice.dropped = 0;
        s_bridge.voice.bytes = 0;
    } else {
        if (!s_bridge.voice.active) {
            bridge_unlock();
            free(item);
            return;
        }
        s_bridge.voice.active = false;
        if (item) {
            item->frames = s_bridge.voice.frames;
            item->dropped = s_bridge.voice.dropped;
            item->gaps = system_ui_workbuddy_audio_mic_gaps();
        }
    }
    if (item) {
        item->session = s_bridge.voice.session;
        if (!voice_enqueue_locked(item)) {
            ESP_LOGW(TAG, "voice event dropped, queue full (session=%u)", item->session);
            free(item);
        }
    }
    bridge_unlock();
}

static void ble_host_bridge_voice_task(void *arg)
{
    (void)arg;

    for (;;) {
        voice_item_t *item = NULL;
        BaseType_t got = xQueueReceive(s_bridge.voice_queue, &item, pdMS_TO_TICKS(200));

        /* Checked on every turn: while packets flow the receive never times out. */
        bridge_lock();
        bool expired = s_bridge.voice.active && s_bridge.voice.max_ms > 0 &&
                       (xTaskGetTickCount() - s_bridge.voice.started_at) >=
                       pdMS_TO_TICKS(s_bridge.voice.max_ms);
        bridge_unlock();
        if (expired) {
            voice_stop_capture("timeout");
        }
        if (got != pdTRUE || !item) {
            continue;
        }

        switch (item->kind) {
        case VOICE_ITEM_AUDIO:
            voice_send_packet(item);
            break;
        case VOICE_ITEM_STARTED:
        case VOICE_ITEM_STOPPED:
            voice_send_event(item);
            break;
        case VOICE_ITEM_STOP_REQUEST:
            voice_stop_capture(item->tag);
            break;
        default:
            break;
        }
        free(item);
    }
}

static uint32_t voice_payload_u32(const cJSON *root, const char *name, uint32_t fallback)
{
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *item = payload && cJSON_IsObject(payload) ?
                        cJSON_GetObjectItemCaseSensitive(payload, name) : NULL;

    if (!item) {
        item = cJSON_GetObjectItemCaseSensitive(root, name);
    }
    return cJSON_IsNumber(item) && item->valuedouble > 0 ? (uint32_t)item->valuedouble : fallback;
}

static bool voice_payload_bool(const cJSON *root, const char *name, bool fallback)
{
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *item = payload && cJSON_IsObject(payload) ?
                        cJSON_GetObjectItemCaseSensitive(payload, name) : NULL;

    if (!item) {
        item = cJSON_GetObjectItemCaseSensitive(root, name);
    }
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void handle_voice_start(const char *id, const cJSON *root)
{
    char data[400];
    uint32_t max_ms = voice_payload_u32(root, "max_ms", BLE_HOST_BRIDGE_VOICE_MAX_MS_DEFAULT);
    bool show_ui = voice_payload_bool(root, "show_ui", false);
    esp_err_t err = ESP_OK;

    if (max_ms > BLE_HOST_BRIDGE_VOICE_MAX_MS_LIMIT) {
        max_ms = BLE_HOST_BRIDGE_VOICE_MAX_MS_LIMIT;
    }
    bridge_lock();
    bool subscribed = s_bridge.audio_subscribed;
    bool active = s_bridge.voice.active;
    if (!active) {
        s_bridge.voice.next_max_ms = max_ms;
    }
    bridge_unlock();

    if (!subscribed) {
        send_response(id, false, "voice_start", "audio_char_not_subscribed");
        return;
    }
    if (!active) {
        if (show_ui) {
            /* Fails while a Lua app owns the display; capture still starts. */
            (void)system_ui_show_workbuddy();
        }
        err = system_ui_workbuddy_start_capture_background("host");
        if (err == ESP_OK) {
            (void)system_ui_update_workbuddy(&(system_ui_workbuddy_update_t) {
                .record = "1",
                .workbuddy_status = "录音中",
            });
        }
    }
    if (err != ESP_OK) {
        bridge_lock();
        s_bridge.voice.next_max_ms = 0;
        bridge_unlock();
        /* The phone app reacts to this by installing the mic script over BLE. */
        send_response(id, false, "voice_start",
                      err == ESP_ERR_NOT_FOUND ? "mic_script_missing" : esp_err_to_name(err));
        return;
    }
    voice_status_json(data, sizeof(data));
    send_response(id, true, "voice_start", data);
}

static void handle_voice_stop(const char *id)
{
    char data[400];

    voice_stop_capture("host");
    voice_status_json(data, sizeof(data));
    send_response(id, true, "voice_stop", data);
}

static void handle_voice_status(const char *id)
{
    char data[400];

    voice_status_json(data, sizeof(data));
    send_response(id, true, "voice_status", data);
}

static int ble_host_bridge_voice_access(uint16_t conn_handle, uint16_t attr_handle,
                                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char status[400];
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    voice_status_json(status, sizeof(status));
    return os_mbuf_append(ctxt->om, status, strlen(status)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* cap_lua_job_status_name() lives in cap_lua's private header, so keep a local
 * mapping. This is only a hint in the pushed frame; the authoritative status
 * comes from the cap_lua_get_job() payload. */
static const char *job_status_label(cap_lua_job_status_t status)
{
    switch (status) {
    case CAP_LUA_JOB_QUEUED:
        return "queued";
    case CAP_LUA_JOB_RUNNING:
        return "running";
    case CAP_LUA_JOB_DONE:
        return "done";
    case CAP_LUA_JOB_FAILED:
        return "failed";
    case CAP_LUA_JOB_TIMEOUT:
        return "timeout";
    case CAP_LUA_JOB_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

/* Pushed to the host when a job reaches a terminal state, so the host learns the
 * result without having to keep polling. Sent with a synthetic request id: the
 * host treats frames whose id has no pending request as unsolicited events. */
static void handle_job_event(const char *job_id, const char *status)
{
    char *output = bridge_allocate_output_buffer();

    if (!job_id || !job_id[0]) {
        free(output);
        return;
    }
    bridge_lock();
    bool deliverable = s_bridge.subscribed && s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE;
    bridge_unlock();
    if (!deliverable || !output) {
        free(output);
        return;
    }

    esp_err_t err = cap_lua_get_job(job_id, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
    if (err != ESP_OK) {
        snprintf(output, BLE_HOST_BRIDGE_OUTPUT_MAX,
                 "job_id=%s\nstatus=%s\nsummary=(job record unavailable)\nrecent_log=(empty)",
                 job_id, status ? status : "unknown");
    }
    send_response(BLE_HOST_BRIDGE_EVENT_REQUEST_ID, true, "job_event", output);
    free(output);
}

/* Runs on the cap_lua runner task. Only allocate and enqueue here: the BLE
 * notify path can block for hundreds of ms and must not stall the runner. */
static void bridge_job_event_cb(const cap_lua_job_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (!event || event->type != CAP_LUA_JOB_EVENT_TERMINAL) {
        return;
    }
    /* Only report jobs this bridge submitted. */
    if (strcmp(event->exclusive, "ble_host") != 0) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "id", BLE_HOST_BRIDGE_EVENT_REQUEST_ID);
    cJSON_AddStringToObject(root, "op", BLE_HOST_BRIDGE_EVENT_OP);
    cJSON_AddStringToObject(root, "job_id", event->job_id);
    cJSON_AddStringToObject(root, "status", job_status_label(event->status));
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return;
    }

    /* Enqueue while holding the lock so a concurrent stop() cannot delete the
     * queue between the check and the send. xQueueSend does not block here. */
    bridge_lock();
    bool queued = false;
    if (s_bridge.subscribed && s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE &&
            s_bridge.request_queue) {
        /* Dropping the push is acceptable: the host also polls job_status. */
        queued = xQueueSend(s_bridge.request_queue, &text, 0) == pdTRUE;
        if (!queued) {
            ESP_LOGW(TAG, "job event dropped, request queue full (job=%s)", event->job_id);
        }
    }
    bridge_unlock();

    if (!queued) {
        free(text);
    }
}

static void dispatch_request(const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    cJSON *id_item;
    cJSON *op_item;
    const char *id;
    const char *op;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        send_response("", false, "error", "invalid_json");
        return;
    }
    id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    op_item = cJSON_GetObjectItemCaseSensitive(root, "op");
    id = cJSON_IsString(id_item) ? id_item->valuestring : "";
    op = cJSON_IsString(op_item) ? op_item->valuestring : "";

    cJSON *home_out = NULL;
    esp_err_t home_err = strcmp(op, "hello") == 0 ? ESP_ERR_NOT_SUPPORTED
                                                  : system_ui_home_handle_op(op, root, false, &home_out);
    if (home_err != ESP_ERR_NOT_SUPPORTED) {
        /* 1.0 首页/模式/应用目录操作：data 为 JSON 字符串 */
        char *text = home_out ? cJSON_PrintUnformatted(home_out) : NULL;
        const cJSON *msg = home_out ? cJSON_GetObjectItemCaseSensitive(home_out, "error") : NULL;
        send_response(id, home_err == ESP_OK, op,
                      home_err == ESP_OK ? (text ? text : "{}")
                                         : (cJSON_IsString(msg) ? msg->valuestring : esp_err_to_name(home_err)));
        cJSON_free(text);
        cJSON_Delete(home_out);
    } else if (strcmp(op, "hello") == 0) {
        handle_hello(id);
    } else if (strncmp(op, "app_", 4) == 0) {
        char *output = NULL;
        esp_err_t err = ble_host_apps_handle(op, root, &output);
        send_response(id, err == ESP_OK, op, err == ESP_OK ? output : esp_err_to_name(err));
        free(output);
    } else if (strcmp(op, "device_info") == 0) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        char *payload_json = payload ? cJSON_PrintUnformatted(payload) : NULL;
        handle_device_info(id, payload_json);
        free(payload_json);
    } else if (strcmp(op, "put_lua") == 0) {
        cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
        handle_put_lua(id, cJSON_IsString(source) ? source->valuestring : "");
    } else if (strcmp(op, "run_lua") == 0) {
        cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "path");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
        cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
        char *args_json = args ? cJSON_PrintUnformatted(args) : NULL;
        handle_run_lua(id,
                       cJSON_IsString(path) ? path->valuestring : "",
                       args_json,
                       cJSON_IsNumber(timeout) ? (uint32_t)timeout->valuedouble : 60000);
        free(args_json);
    } else if (strcmp(op, "job_status") == 0) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
        if (!cJSON_IsString(job_id) && payload && cJSON_IsObject(payload)) {
            job_id = cJSON_GetObjectItemCaseSensitive(payload, "job_id");
        }
        handle_job_status(id, cJSON_IsString(job_id) ? job_id->valuestring : NULL);
    } else if (strcmp(op, "job_tail") == 0) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
        cJSON *since = cJSON_GetObjectItemCaseSensitive(root, "since_seq");
        cJSON *max_bytes = cJSON_GetObjectItemCaseSensitive(root, "max_bytes");
        if (!cJSON_IsString(job_id) && payload && cJSON_IsObject(payload)) {
            job_id = cJSON_GetObjectItemCaseSensitive(payload, "job_id");
        }
        handle_job_tail(id,
                        cJSON_IsString(job_id) ? job_id->valuestring : NULL,
                        cJSON_IsNumber(since) && since->valuedouble > 0 ?
                        (uint64_t)since->valuedouble : 0,
                        cJSON_IsNumber(max_bytes) ? (uint32_t)max_bytes->valuedouble : 0);
    } else if (strcmp(op, "job_stop") == 0) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
        if (!cJSON_IsString(job_id) && payload && cJSON_IsObject(payload)) {
            job_id = cJSON_GetObjectItemCaseSensitive(payload, "job_id");
        }
        handle_job_stop(id, cJSON_IsString(job_id) ? job_id->valuestring : NULL);
    } else if (strcmp(op, "workbuddy_ui") == 0) {
        handle_workbuddy_ui(id, root);
    } else if (strcmp(op, "workbuddy_audio_control") == 0) {
        handle_workbuddy_audio_control(id, root);
    } else if (strcmp(op, "workbuddy_audio_opus") == 0) {
        handle_workbuddy_audio_opus(id, root);
    } else if (strcmp(op, "voice_start") == 0) {
        handle_voice_start(id, root);
    } else if (strcmp(op, "voice_stop") == 0) {
        handle_voice_stop(id);
    } else if (strcmp(op, "voice_status") == 0) {
        handle_voice_status(id);
    } else if (strcmp(op, BLE_HOST_BRIDGE_EVENT_OP) == 0) {
        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
        cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        handle_job_event(cJSON_IsString(job_id) ? job_id->valuestring : NULL,
                         cJSON_IsString(status) ? status->valuestring : NULL);
    } else {
        send_response(id, false, "error", "unknown_op");
    }
    cJSON_Delete(root);
}

static void handle_rx_text(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    cJSON *kind = root ? cJSON_GetObjectItemCaseSensitive(root, "kind") : NULL;

    if (!root) {
        send_response("", false, "error", "invalid_json");
        return;
    }

    if ((cJSON_IsString(kind) && strcmp(kind->valuestring, "chunk") == 0) ||
        (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "k")) &&
         strcmp(cJSON_GetObjectItemCaseSensitive(root, "k")->valuestring, "c") == 0)) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
        cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        cJSON *id_short = cJSON_GetObjectItemCaseSensitive(root, "i");
        cJSON *op_short = cJSON_GetObjectItemCaseSensitive(root, "o");
        cJSON *seq_short = cJSON_GetObjectItemCaseSensitive(root, "s");
        cJSON *total_short = cJSON_GetObjectItemCaseSensitive(root, "t");
        cJSON *data_short = cJSON_GetObjectItemCaseSensitive(root, "d");

        if (!cJSON_IsString(id) && cJSON_IsString(id_short)) {
            id = id_short;
        }
        if (!cJSON_IsString(op) && cJSON_IsString(op_short)) {
            op = op_short;
        }
        if (!cJSON_IsNumber(seq) && cJSON_IsNumber(seq_short)) {
            seq = seq_short;
        }
        if (!cJSON_IsNumber(total) && cJSON_IsNumber(total_short)) {
            total = total_short;
        }
        if (!cJSON_IsString(data) && cJSON_IsString(data_short)) {
            data = data_short;
        }

        if (!cJSON_IsString(id) || !cJSON_IsString(op) || !cJSON_IsNumber(seq) ||
                !cJSON_IsNumber(total) || !cJSON_IsString(data)) {
            cJSON_Delete(root);
            send_response("", false, "error", "invalid_chunk");
            return;
        }

        bridge_lock();
        if ((uint16_t)seq->valuedouble == 0) {
            reset_request_locked();
            strlcpy(s_bridge.request_id, id->valuestring, sizeof(s_bridge.request_id));
            strlcpy(s_bridge.pending_op, op->valuestring, sizeof(s_bridge.pending_op));
            s_bridge.expected_chunks = (uint16_t)total->valuedouble;
            s_bridge.state = BLE_HOST_BRIDGE_RECEIVING;
        }
        size_t data_len = strlen(data->valuestring);
        bool valid = strcmp(s_bridge.request_id, id->valuestring) == 0 &&
                     strcmp(s_bridge.pending_op, op->valuestring) == 0 &&
                     s_bridge.received_chunks == (uint16_t)seq->valuedouble &&
                     s_bridge.request_len + data_len < BLE_HOST_BRIDGE_REQUEST_MAX;
        if (valid) {
            memcpy(s_bridge.request_buf + s_bridge.request_len, data->valuestring, data_len);
            s_bridge.request_len += data_len;
            s_bridge.request_buf[s_bridge.request_len] = '\0';
            s_bridge.received_chunks++;
        }
        bool complete = valid && s_bridge.received_chunks == s_bridge.expected_chunks;
        char *request_copy = complete ? strdup(s_bridge.request_buf) : NULL;
        char req_id[sizeof(s_bridge.request_id)];
        strlcpy(req_id, s_bridge.request_id, sizeof(req_id));
        if (complete || !valid) {
            reset_request_locked();
        }
        bridge_unlock();

        cJSON_Delete(root);
        if (!valid) {
            send_response(req_id, false, "error", "chunk_sequence_error_or_too_large");
        } else if (complete && request_copy) {
            /* Reassembled data is the original JSON request, not Lua source. */
            dispatch_request(request_copy);
            free(request_copy);
        }
        return;
    }

    cJSON_Delete(root);
    dispatch_request(text);
}

static void ble_host_bridge_request_task(void *arg)
{
    (void)arg;
    char *request = NULL;

    while (xQueueReceive(s_bridge.request_queue, &request, portMAX_DELAY) == pdTRUE) {
        if (request != NULL) {
            handle_rx_text(request);
            free(request);
        }
    }
    s_bridge.request_task = NULL;
    vTaskDelete(NULL);
}

static int ble_host_bridge_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* Writing RX identifies this peer as the host tool, same as subscribing to
     * TX. A HID peer never touches this characteristic. */
    bridge_lock();
    if (s_bridge.conn_handle != conn_handle) {
        s_bridge.conn_handle = conn_handle;
        if (s_bridge.state == BLE_HOST_BRIDGE_IDLE) {
            s_bridge.state = BLE_HOST_BRIDGE_CONNECTED;
        }
    }
    bridge_unlock();

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > BLE_HOST_BRIDGE_REQUEST_MAX) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char *buf = calloc(1, (size_t)len + 1);
    if (!buf) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    if (rc != 0) {
        free(buf);
        return BLE_ATT_ERR_UNLIKELY;
    }

    /* NimBLE callbacks must not execute Lua, touch LVGL, or notify. */
    if (xQueueSend(s_bridge.request_queue, &buf, 0) != pdTRUE) {
        free(buf);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int ble_host_bridge_status_access(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    char status[512];
    bridge_lock();
    snprintf(status, sizeof(status),
             "{\"state\":\"%s\",\"subscribed\":%s,\"last_job_id\":\"%.*s\",\"storage\":\"%.*s\"}",
             state_name(s_bridge.state),
             s_bridge.subscribed ? "true" : "false",
             64, s_bridge.last_job_id,
             64, s_bridge.storage_base_path);
    bridge_unlock();
    return os_mbuf_append(ctxt->om, status, strlen(status)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_host_bridge_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        bridge_lock();
        if (event->connect.status == 0) {
            if (s_bridge.conn_count < UINT8_MAX) {
                s_bridge.conn_count++;
            }
            /* All connections arrive through our single advertising set, HID
             * peers included, so only adopt one if we have no peer yet. A peer
             * that subscribes to TX or writes RX identifies itself as the real
             * host tool and takes over from there. */
            if (s_bridge.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                s_bridge.conn_handle = event->connect.conn_handle;
                s_bridge.state = BLE_HOST_BRIDGE_CONNECTED;
            }
            ESP_LOGI(TAG, "peer connected conn_handle=%u conns=%u",
                     event->connect.conn_handle, s_bridge.conn_count);
        }
        bridge_unlock();
        /* Keep advertising while a slot is free; HID and the host tool each
         * need their own link. */
        ble_host_bridge_advertise_or_retry();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        bridge_lock();
        if (s_bridge.conn_count > 0) {
            s_bridge.conn_count--;
        }
        /* Only tear down bridge state when the peer that left was ours. */
        if (event->disconnect.conn.conn_handle == s_bridge.conn_handle) {
            ESP_LOGI(TAG, "host disconnected reason=%d", event->disconnect.reason);
            s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_bridge.subscribed = false;
            s_bridge.audio_subscribed = false;
            s_bridge.negotiated_mtu = BLE_HOST_BRIDGE_PREFERRED_MTU;
            if (s_bridge.voice.active) {
                /* Nobody is listening any more; stop on the voice task. */
                voice_item_t *stop = voice_item_new(VOICE_ITEM_STOP_REQUEST, "disconnect");
                if (stop && !voice_enqueue_locked(stop)) {
                    free(stop);
                }
            }
            reset_request_locked();
            s_bridge.state = BLE_HOST_BRIDGE_IDLE;
        } else {
            ESP_LOGI(TAG, "peer disconnected conn_handle=%u reason=%d conns=%u",
                     event->disconnect.conn.conn_handle, event->disconnect.reason,
                     s_bridge.conn_count);
        }
        bridge_unlock();
        ble_host_bridge_advertise_or_retry();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Let the HID listener handle its own peer; we only need to track
         * whether our host-tool peer's link is encrypted so we can gate
         * notifications.  For any other peer this is a no-op. */
        ESP_LOGI(TAG, "enc_change conn_handle=%u status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Both the HID peer and the host-tool peer may trigger this when
         * their stored keys don't match ours.  Delete the stale bond and
         * allow NimBLE to retry; without this Windows silently fails. */
        ESP_LOGW(TAG, "repeat_pairing conn_handle=%u — deleting stale bond",
                 event->repeat_pairing.conn_handle);
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_bridge.tx_val_handle) {
            bridge_lock();
            /* Subscribing to TX proves this is the host tool, not a HID peer. */
            s_bridge.conn_handle = event->subscribe.conn_handle;
            s_bridge.subscribed = event->subscribe.cur_notify || event->subscribe.cur_indicate;
            if (s_bridge.state == BLE_HOST_BRIDGE_IDLE) {
                s_bridge.state = BLE_HOST_BRIDGE_CONNECTED;
            }
            bridge_unlock();
            ESP_LOGI(TAG, "host notify subscription=%d conn_handle=%u",
                     s_bridge.subscribed, event->subscribe.conn_handle);
        } else if (event->subscribe.attr_handle == s_bridge.audio_val_handle) {
            bridge_lock();
            s_bridge.conn_handle = event->subscribe.conn_handle;
            s_bridge.audio_subscribed = event->subscribe.cur_notify;
            if (s_bridge.state == BLE_HOST_BRIDGE_IDLE) {
                s_bridge.state = BLE_HOST_BRIDGE_CONNECTED;
            }
            if (!s_bridge.audio_subscribed && s_bridge.voice.active) {
                voice_item_t *stop = voice_item_new(VOICE_ITEM_STOP_REQUEST, "unsubscribed");
                if (stop && !voice_enqueue_locked(stop)) {
                    free(stop);
                }
            }
            bridge_unlock();
            ESP_LOGI(TAG, "host audio subscription=%d conn_handle=%u",
                     s_bridge.audio_subscribed, event->subscribe.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        /* Response chunking is sized from this, so only track our own peer. */
        bridge_lock();
        if (event->mtu.conn_handle == s_bridge.conn_handle) {
            s_bridge.negotiated_mtu = event->mtu.value;
        }
        bridge_unlock();
        ESP_LOGI(TAG, "mtu conn_handle=%u value=%u", event->mtu.conn_handle, event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_host_bridge_advertise_or_retry();
        return 0;
    default:
        return 0;
    }
}

static void ble_host_bridge_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
}

static esp_timer_handle_t s_adv_retry_timer;
static int s_adv_retry_count;

static void ble_host_bridge_adv_retry_cb(void *arg)
{
    (void)arg;
    /* 已在广播或连接槽已满时 start_advertising 直接返回 OK */
    if (ble_host_bridge_start_advertising() == ESP_OK) {
        ESP_LOGI(TAG, "advertising started after %d retries", s_adv_retry_count);
        s_adv_retry_count = 0;
        return;
    }
    if (++s_adv_retry_count < 20 && s_adv_retry_timer) {
        (void)esp_timer_start_once(s_adv_retry_timer, 3000 * 1000);
    }
}

static void ble_host_bridge_schedule_adv_retry(void)
{
    if (!s_adv_retry_timer) {
        const esp_timer_create_args_t args = {.callback = ble_host_bridge_adv_retry_cb, .name = "ble_adv_retry"};
        if (esp_timer_create(&args, &s_adv_retry_timer) != ESP_OK) {
            return;
        }
    }
    (void)esp_timer_stop(s_adv_retry_timer);
    (void)esp_timer_start_once(s_adv_retry_timer, 3000 * 1000);
}

static void ble_host_bridge_advertise_or_retry(void)
{
    if (ble_host_bridge_start_advertising() != ESP_OK) {
        ble_host_bridge_schedule_adv_retry();
    }
}

static void ble_host_bridge_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_bridge.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
    }
    s_bridge.host_synced = true;
    s_bridge.negotiated_mtu = BLE_HOST_BRIDGE_PREFERRED_MTU;
    s_bridge.startup_err = ble_host_bridge_start_advertising();
    if (s_bridge.sync_sem) {
        xSemaphoreGive(s_bridge.sync_sem);
    }
}

static void ble_host_bridge_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// #include "esp_mac.h"

static esp_err_t start_controller(void)
{
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    esp_err_t err;

    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        // uint8_t base_mac[6] = {0x02,0x22,0x33,0x44,0x55,0x68};
        // esp_base_mac_addr_set(base_mac);
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&bt_cfg);
        ESP_RETURN_ON_ERROR(err, TAG, "controller init failed");
        s_bridge.controller_inited = true;
        status = esp_bt_controller_get_status();
    }
    if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        ESP_RETURN_ON_ERROR(err, TAG, "controller enable failed");
        s_bridge.controller_enabled = true;
    }
    return ESP_OK;
}

static esp_err_t ble_host_bridge_start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    char short_name[sizeof(BLE_HOST_BRIDGE_SHORT_NAME_PREFIX) + BLE_HOST_BRIDGE_MAC_SUFFIX_LEN + 1] = {0};
    int rc;

    if (ble_gap_adv_active()) {
        return ESP_OK;
    }
    /* The controller rejects advertising once every connection slot is taken;
     * a disconnect event will bring us back here. */
    if (s_bridge.conn_count >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        ESP_LOGI(TAG, "all %d connection slots in use, advertising paused",
                 CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
        return ESP_OK;
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
    static const ble_uuid16_t s_hid_uuid16 = BLE_UUID16_INIT(LUA_MODULE_BLE_HID_UUID16);

    if (lua_module_ble_hid_is_attached()) {
        /* HID mode: primary ADV carries flags + appearance + 0x1812 + shortened name.
         * Windows HID pairing needs the device name visible in the primary ADV packet
         * (not just scan response) to trigger the HID pairing UI.
         * Budget: flags 3 + appearance 4 + uuid16 4 + name "esp-claw-XXXX" 14 = 25 bytes. */
        if (make_device_name_with_mac_suffix(BLE_HOST_BRIDGE_SHORT_NAME_PREFIX,
                                             short_name,
                                             sizeof(short_name)) != ESP_OK) {
            strlcpy(short_name, BLE_HOST_BRIDGE_SHORT_NAME_PREFIX, sizeof(short_name));
        }
        fields.appearance = lua_module_ble_hid_appearance();
        fields.appearance_is_present = 1;
        fields.uuids16 = &s_hid_uuid16;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
        /* Shortened local name in primary ADV — full name in scan response below. */
        fields.name = (const uint8_t *)short_name;
        fields.name_len = strlen(short_name);
        fields.name_is_complete = 0;
    } else {
        fields.uuids128 = &s_service_uuid;
        fields.num_uuids128 = 1;
        fields.uuids128_is_complete = 1;
    }
#else
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
#endif
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields failed rc=%d", rc);
        return ESP_FAIL;
    }

    rsp_fields.name = (const uint8_t *)s_bridge.device_name;
    rsp_fields.name_len = strlen(s_bridge.device_name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response fields failed rc=%d", rc);
        return ESP_FAIL;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = adv_ms_to_units(BLE_HOST_BRIDGE_ADV_MIN_MS);
    adv_params.itvl_max = adv_ms_to_units(BLE_HOST_BRIDGE_ADV_MAX_MS);
    rc = ble_gap_adv_start(s_bridge.own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_host_bridge_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv start failed rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_host_bridge_start(const ble_host_bridge_config_t *config)
{
    esp_err_t err;
    esp_err_t ret = ESP_OK;
    const char *base_device_name;
    int rc;

    if (s_bridge.started) {
        return ESP_OK;
    }

    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    base_device_name = config && config->device_name && config->device_name[0] ?
                       config->device_name : BLE_HOST_BRIDGE_DEFAULT_NAME;
    err = make_device_name_with_mac_suffix(base_device_name,
                                           s_bridge.device_name,
                                           sizeof(s_bridge.device_name));
    ESP_RETURN_ON_ERROR(err, TAG, "build device name failed");
    strlcpy(s_bridge.storage_base_path,
            config && config->storage_base_path && config->storage_base_path[0] ?
            config->storage_base_path : "/fatfs",
            sizeof(s_bridge.storage_base_path));

    s_bridge.request_buf = calloc(1, BLE_HOST_BRIDGE_REQUEST_MAX + 1);
    ESP_RETURN_ON_FALSE(s_bridge.request_buf, ESP_ERR_NO_MEM, TAG, "request buffer alloc failed");
    s_bridge.mutex = xSemaphoreCreateMutex();
    s_bridge.sync_sem = xSemaphoreCreateBinary();
    s_bridge.request_queue = xQueueCreate(BLE_HOST_BRIDGE_REQUEST_QUEUE_LEN, sizeof(char *));
    ESP_GOTO_ON_FALSE(s_bridge.mutex && s_bridge.sync_sem && s_bridge.request_queue,
                      ESP_ERR_NO_MEM, fail, TAG, "runtime object alloc failed");
    s_bridge.voice_queue = xQueueCreate(BLE_HOST_BRIDGE_VOICE_QUEUE_LEN, sizeof(voice_item_t *));
    ESP_GOTO_ON_FALSE(s_bridge.voice_queue, ESP_ERR_NO_MEM, fail, TAG, "voice queue alloc failed");
    BaseType_t task_ok = xTaskCreate(ble_host_bridge_request_task,
                                     "ble_bridge_req",
                                     BLE_HOST_BRIDGE_REQUEST_TASK_STACK,
                                     NULL,
                                     5,
                                     &s_bridge.request_task);
    ESP_GOTO_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG,
                      "request task create failed");
    /* Same priority as the request task: below NimBLE's host task, above the
     * capture task (3), so queued packets drain as soon as the radio allows. */
    task_ok = xTaskCreate(ble_host_bridge_voice_task,
                          "ble_bridge_voice",
                          BLE_HOST_BRIDGE_VOICE_TASK_STACK,
                          NULL,
                          5,
                          &s_bridge.voice_task);
    ESP_GOTO_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG,
                      "voice task create failed");

    err = nvs_flash_init();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    ESP_GOTO_ON_ERROR(err, fail, TAG, "nvs init failed");
    ESP_GOTO_ON_ERROR(start_controller(), fail, TAG, "controller start failed");
    ESP_GOTO_ON_ERROR(esp_nimble_init(), fail, TAG, "nimble init failed");
    rc = ble_att_set_preferred_mtu(BLE_HOST_BRIDGE_PREFERRED_MTU);
    ESP_GOTO_ON_FALSE(rc == 0, ESP_FAIL, fail, TAG, "set preferred mtu failed rc=%d", rc);

    /* Install our callbacks before attaching HID: esp_hidd_dev_init() chains
     * whatever sync_cb/reset_cb it finds, so ours have to already be in place. */
    ble_hs_cfg.reset_cb = ble_host_bridge_on_reset;
    ble_hs_cfg.sync_cb = ble_host_bridge_on_sync;
    /* Evict the oldest bond when the store is full instead of failing with
     * BLE_HS_ESTORE_CAP, which would leave the local IRK un-persisted and
     * cause the device identity to change on every reboot. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    bool hid_owns_base_svcs = false;
#if CONFIG_APP_CLAW_LUA_MODULE_BLE_HID
    /* Register the HID services into this same GATT server. NimBLE only accepts
     * services while the database is still mutable, i.e. before the host task
     * starts and advertising begins, so this cannot be deferred to Lua. */
    err = lua_module_ble_hid_attach(&(lua_module_ble_hid_attach_config_t) {
        .device_name = s_bridge.device_name,
    });
    if (err == ESP_OK) {
        /* esp_hidd_dev_init() already ran ble_svc_gap_init()/ble_svc_gatt_init();
         * ble_svc_gap_init() adds unconditionally, so calling it again would
         * register the GAP service twice. */
        hid_owns_base_svcs = true;
    } else {
        /* Losing HID must not take the host bridge down with it. */
        ESP_LOGW(TAG, "BLE HID attach failed, continuing without HID: %s",
                 esp_err_to_name(err));
        err = ESP_OK;
    }
#endif
    if (!hid_owns_base_svcs) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
    }
    rc = ble_svc_gap_device_name_set(s_bridge.device_name);
    ESP_GOTO_ON_FALSE(rc == 0, ESP_FAIL, fail, TAG, "set device name failed rc=%d", rc);
    rc = ble_gatts_count_cfg(s_bridge_svcs);
    ESP_GOTO_ON_FALSE(rc == 0, ESP_FAIL, fail, TAG, "gatt count failed rc=%d", rc);
    rc = ble_gatts_add_svcs(s_bridge_svcs);
    ESP_GOTO_ON_FALSE(rc == 0, ESP_FAIL, fail, TAG, "gatt add failed rc=%d", rc);

    nimble_port_freertos_init(ble_host_bridge_host_task);
    if (xSemaphoreTake(s_bridge.sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_GOTO_ON_FALSE(false, ESP_ERR_TIMEOUT, fail, TAG, "host sync timeout");
    }
    if (s_bridge.startup_err != ESP_OK) {
        /* 1.0：首次广播失败（例如控制器一时内存不足）不再拆掉整个蓝牙栈——
         * 拆栈时控制器事件池未清空会直接断言重启。改为定时重试广播。 */
        ESP_LOGW(TAG, "advertising start failed at boot, retrying in background");
        ble_host_bridge_schedule_adv_retry();
    }

    /* Non-fatal: without it the host falls back to polling job_status. */
    err = cap_lua_register_job_event_cb(bridge_job_event_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "job event subscribe failed: %s", esp_err_to_name(err));
    }
    (void)system_ui_workbuddy_set_audio_tx_callback(ble_host_bridge_send_workbuddy_audio, NULL);
    (void)system_ui_workbuddy_set_audio_state_callback(bridge_voice_state_cb, NULL);

    s_bridge.started = true;
    ESP_LOGI(TAG, "started name=%s storage=%s", s_bridge.device_name, s_bridge.storage_base_path);
    return ESP_OK;

fail:
    ble_host_bridge_stop();
    return ret != ESP_OK ? ret : (err == ESP_OK ? ESP_FAIL : err);
}

esp_err_t ble_host_bridge_stop(void)
{
    /* Unsubscribe first: a job event arriving later must not enqueue into a
     * queue that is about to be deleted. */
    (void)cap_lua_unregister_job_event_cb(bridge_job_event_cb, NULL);
    (void)system_ui_workbuddy_set_audio_tx_callback(NULL, NULL);
    (void)system_ui_workbuddy_set_audio_state_callback(NULL, NULL);

    if (s_bridge.request_task) {
        vTaskDelete(s_bridge.request_task);
        s_bridge.request_task = NULL;
    }
    if (s_bridge.voice_task) {
        vTaskDelete(s_bridge.voice_task);
        s_bridge.voice_task = NULL;
    }
    /* Detach the queue under the lock so no producer can still see it, then
     * drain and delete it. */
    bridge_lock();
    QueueHandle_t request_queue = s_bridge.request_queue;
    s_bridge.request_queue = NULL;
    bridge_unlock();
    if (request_queue) {
        char *request = NULL;
        while (xQueueReceive(request_queue, &request, 0) == pdTRUE) {
            free(request);
        }
        vQueueDelete(request_queue);
    }
    bridge_lock();
    QueueHandle_t voice_queue = s_bridge.voice_queue;
    s_bridge.voice_queue = NULL;
    bridge_unlock();
    if (voice_queue) {
        voice_item_t *item = NULL;
        while (xQueueReceive(voice_queue, &item, 0) == pdTRUE) {
            free(item);
        }
        vQueueDelete(voice_queue);
    }
    if (s_bridge.started) {
        (void)ble_gap_adv_stop();
        (void)nimble_port_stop();
        (void)esp_nimble_deinit();
    }
    if (s_bridge.controller_enabled) {
        (void)esp_bt_controller_disable();
    }
    if (s_bridge.controller_inited) {
        (void)esp_bt_controller_deinit();
    }
    if (s_bridge.mutex) {
        vSemaphoreDelete(s_bridge.mutex);
    }
    if (s_bridge.sync_sem) {
        vSemaphoreDelete(s_bridge.sync_sem);
    }
    free(s_bridge.request_buf);
    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    return ESP_OK;
}

/* 1.0 首页状态来源：手机连着且订阅了语音特征时，背夹模式的语音可用。 */
bool ble_host_bridge_voice_linked(void)
{
    return s_bridge.audio_subscribed && s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_host_bridge_connected(void)
{
    return s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

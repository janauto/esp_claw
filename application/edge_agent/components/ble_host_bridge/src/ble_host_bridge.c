/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ble_host_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "cap_lua.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
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
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
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
#define BLE_HOST_BRIDGE_REQUEST_QUEUE_LEN 4
#define BLE_HOST_BRIDGE_REQUEST_TASK_STACK 8192

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
    uint8_t own_addr_type;
    uint16_t conn_handle;
    uint16_t tx_val_handle;
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
    ble_host_bridge_state_t state;
} ble_host_bridge_ctx_t;

static ble_host_bridge_ctx_t s_bridge;

static int ble_host_bridge_gap_event(struct ble_gap_event *event, void *arg);
static int ble_host_bridge_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_host_bridge_status_access(uint16_t conn_handle, uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static esp_err_t ble_host_bridge_start_advertising(void);
static void handle_hello(const char *id);
static void handle_device_info(const char *id, const char *payload_json);
static void handle_put_lua(const char *id, const char *source);
static void handle_run_lua(const char *id, const char *path, const char *args_json, uint32_t timeout_ms);
static void handle_job_status(const char *id, const char *job_id);
static void handle_job_stop(const char *id, const char *job_id);
static void ble_host_bridge_request_task(void *arg);

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

    size_t total = (len + chunk_size - 1) / chunk_size;
    for (size_t seq = 0; seq < total; seq++) {
        size_t off = seq * chunk_size;
        size_t chunk = len - off;
        if (chunk > chunk_size) {
            chunk = chunk_size;
        }
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
    char data[256];
    snprintf(data, sizeof(data),
             "{\"protocol\":%d,\"device_name\":\"%s\",\"ops\":[\"hello\",\"device_info\",\"put_lua\",\"run_lua\",\"job_status\",\"job_stop\"]}",
             BLE_HOST_BRIDGE_PROTOCOL_VERSION, s_bridge.device_name);
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

    err = cap_lua_run_script_async(path,
                                   args_json && args_json[0] ? args_json : "{}",
                                   timeout_ms ? timeout_ms : 60000,
                                   id && id[0] ? id : "ble-host",
                                   "ble_host",
                                   true,
                                   output,
                                   BLE_HOST_BRIDGE_OUTPUT_MAX);
    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(output);
        cJSON *job_id = root ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
        bridge_lock();
        if (cJSON_IsString(job_id) && job_id->valuestring) {
            strlcpy(s_bridge.last_job_id, job_id->valuestring, sizeof(s_bridge.last_job_id));
        }
        s_bridge.state = s_bridge.conn_handle != BLE_HS_CONN_HANDLE_NONE ?
                         BLE_HOST_BRIDGE_CONNECTED : BLE_HOST_BRIDGE_IDLE;
        bridge_unlock();
        cJSON_Delete(root);
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
    err = cap_lua_stop_job(job_id, 3000, output, BLE_HOST_BRIDGE_OUTPUT_MAX);
    send_response(id, err == ESP_OK, "job_stop", err == ESP_OK ? output : esp_err_to_name(err));
    free(output);
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

    if (strcmp(op, "hello") == 0) {
        handle_hello(id);
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
    } else if (strcmp(op, "job_stop") == 0) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *job_id = cJSON_GetObjectItemCaseSensitive(root, "job_id");
        if (!cJSON_IsString(job_id) && payload && cJSON_IsObject(payload)) {
            job_id = cJSON_GetObjectItemCaseSensitive(payload, "job_id");
        }
        handle_job_stop(id, cJSON_IsString(job_id) ? job_id->valuestring : NULL);
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
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

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
            s_bridge.conn_handle = event->connect.conn_handle;
            s_bridge.state = BLE_HOST_BRIDGE_CONNECTED;
            ESP_LOGI(TAG, "host connected conn_handle=%u", s_bridge.conn_handle);
        } else {
            s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_bridge.state = BLE_HOST_BRIDGE_IDLE;
            (void)ble_host_bridge_start_advertising();
        }
        bridge_unlock();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "host disconnected reason=%d", event->disconnect.reason);
        bridge_lock();
        s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_bridge.subscribed = false;
        reset_request_locked();
        s_bridge.state = BLE_HOST_BRIDGE_IDLE;
        bridge_unlock();
        (void)ble_host_bridge_start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_bridge.tx_val_handle) {
            bridge_lock();
            s_bridge.subscribed = event->subscribe.cur_notify || event->subscribe.cur_indicate;
            bridge_unlock();
            ESP_LOGI(TAG, "host notify subscription=%d", s_bridge.subscribed);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_bridge.negotiated_mtu = event->mtu.value;
        ESP_LOGI(TAG, "host mtu=%u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        (void)ble_host_bridge_start_advertising();
        return 0;
    default:
        return 0;
    }
}

static void ble_host_bridge_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
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

static esp_err_t start_controller(void)
{
    esp_bt_controller_status_t status = esp_bt_controller_get_status();
    esp_err_t err;

    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
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
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
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
    int rc;

    if (s_bridge.started) {
        return ESP_OK;
    }

    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    strlcpy(s_bridge.device_name,
            config && config->device_name && config->device_name[0] ?
            config->device_name : BLE_HOST_BRIDGE_DEFAULT_NAME,
            sizeof(s_bridge.device_name));
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
    BaseType_t task_ok = xTaskCreate(ble_host_bridge_request_task,
                                     "ble_bridge_req",
                                     BLE_HOST_BRIDGE_REQUEST_TASK_STACK,
                                     NULL,
                                     5,
                                     &s_bridge.request_task);
    ESP_GOTO_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG,
                      "request task create failed");

    err = nvs_flash_init();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    ESP_GOTO_ON_ERROR(err, fail, TAG, "nvs init failed");
    ESP_GOTO_ON_ERROR(start_controller(), fail, TAG, "controller start failed");
    ESP_GOTO_ON_ERROR(esp_nimble_init(), fail, TAG, "nimble init failed");
    rc = ble_att_set_preferred_mtu(BLE_HOST_BRIDGE_PREFERRED_MTU);
    ESP_GOTO_ON_FALSE(rc == 0, ESP_FAIL, fail, TAG, "set preferred mtu failed rc=%d", rc);

    ble_hs_cfg.reset_cb = ble_host_bridge_on_reset;
    ble_hs_cfg.sync_cb = ble_host_bridge_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
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
    ESP_GOTO_ON_ERROR(s_bridge.startup_err, fail, TAG, "advertising start failed");

    s_bridge.started = true;
    ESP_LOGI(TAG, "started name=%s storage=%s", s_bridge.device_name, s_bridge.storage_base_path);
    return ESP_OK;

fail:
    ble_host_bridge_stop();
    return ret != ESP_OK ? ret : (err == ESP_OK ? ESP_FAIL : err);
}

esp_err_t ble_host_bridge_stop(void)
{
    if (s_bridge.request_task) {
        vTaskDelete(s_bridge.request_task);
        s_bridge.request_task = NULL;
    }
    if (s_bridge.request_queue) {
        char *request = NULL;
        while (xQueueReceive(s_bridge.request_queue, &request, 0) == pdTRUE) {
            free(request);
        }
        vQueueDelete(s_bridge.request_queue);
        s_bridge.request_queue = NULL;
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
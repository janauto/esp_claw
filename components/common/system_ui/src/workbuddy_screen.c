/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * workbuddy — the native screen behind the "WorkBuddy" carousel page.
 *
 * Ported from tools/add_ui/ui_Screen6.c, a SquareLine Studio export. The layout
 * is the generated one, but two things are deliberately different:
 *
 *  - Text is drawn with lv_label on the system tiny_ttf font instead of
 *    lv_textarea on ui_font_Font1. The bitmap font carries only the four CJK
 *    glyphs 会议纪要, so every other string on this screen (等待中 / 未关联 /
 *    开始录音) would have rendered as blanks. Labels also make the generated
 *    cursor/selected-part styling unnecessary, which is most of what the
 *    original file spent its lines on.
 *
 *  - The screen is a shared-LVGL screen owned by system_ui, not an exclusive
 *    display session. The physical keys stay live, and returning to the
 *    carousel is a screen swap rather than a session release.
 *
 * It lives in system_ui rather than ui_squareline because the system font and
 * the display session belong to system_ui, and system_ui already depends on
 * ui_squareline rather than the other way round.
 */
#include "system_ui_private.h"
#include "system_ui_audio.h"
#include "claw_paths.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_board_manager.h"
#include "esp_heap_caps.h"
#include "dev_audio_codec.h"
#include "audio_codec_data_if.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "lua_module_audio.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* Forward declaration for cap_lua_async API */
typedef struct {
    char path[256];
    char name[64];
    char exclusive[32];
    char *args_json;
    uint32_t timeout_ms;
    size_t log_bytes;
    bool replace;
    bool sync_waiter;
    time_t created_at;
} cap_lua_async_job_t;

extern esp_err_t cap_lua_async_submit(const cap_lua_async_job_t *job,
                                      char *job_id_out,
                                      size_t job_id_out_size,
                                      char *err_out,
                                      size_t err_out_size);

extern esp_err_t cap_lua_run_script_async(const char *path,
                                          const char *args_json,
                                          uint32_t timeout_ms,
                                          const char *name,
                                          const char *exclusive,
                                          bool replace,
                                          char *output,
                                          size_t output_size);

#define PA_CTRL_GPIO 7

#define AUDIO_LOOPBACK_TASK_STACK_SIZE 49152
#define AUDIO_LOOPBACK_TASK_PRIORITY   3
#define AUDIO_LOOPBACK_BUFFER_SIZE     4096
#define AUDIO_DATA_QUEUE_SIZE          10    /* Queue for passing audio data to BLE */
#define AUDIO_LOOPBACK_WAIT_MS         20
#define AUDIO_SHARED_MIC_AREA          "workbuddy_mic"
#define AUDIO_SHARED_SPK_AREA          "workbuddy_spk"
#define AUDIO_OPUS_SAMPLE_RATE         16000
#define AUDIO_OPUS_CHANNELS            1
#define AUDIO_OPUS_BITS                16
#define AUDIO_OPUS_BITRATE             24000
#define AUDIO_OPUS_FRAME_MS            60
#define AUDIO_OPUS_PCM_FRAME_BYTES     ((AUDIO_OPUS_SAMPLE_RATE / 1000) * AUDIO_OPUS_FRAME_MS * AUDIO_OPUS_CHANNELS * (AUDIO_OPUS_BITS / 8))

/* Control files for headless Lua audio script */
#define AUDIO_CONTROL_FILE  "/ramfs/workbuddy_audio_control"
#define AUDIO_RUNNING_FLAG  "/ramfs/workbuddy_audio_running"
#define AUDIO_SCRIPT_PATH   "skills/audio_loopback_test/scripts/audio_loopback_headless.lua"
/* Units flashed app-only keep an older SYSTEM image that may lack the script
 * above; a host can install the same script here over BLE (put_lua id
 * "voice_mic"). */
#define AUDIO_SCRIPT_DATA_FALLBACK "scripts/host/voice_mic.lua"

/* Forward declaration of ui_squareline_launch_app */
extern void ui_squareline_launch_app(const char *script_rel_path);
#include "ui_squareline.h"

/* Same chrome geometry as the carousel screen: a full 480x800 portrait root. */
#define WORKBUDDY_ROOT_W        480
#define WORKBUDDY_ROOT_H        800
#define WORKBUDDY_ROOT_Y        0

#define WORKBUDDY_C_BG          0x000000
#define WORKBUDDY_C_PANEL       0x080C08
#define WORKBUDDY_C_ACCENT      0xF0C430
#define WORKBUDDY_C_ACCENT_HI   0xF8E458
#define WORKBUDDY_C_DOT         0xF8AC10
#define WORKBUDDY_C_TEXT        0xFAFBF5
#define WORKBUDDY_C_TEXT_DIM    0x848A82
#define WORKBUDDY_C_TEXT_OK     0xC7DD51
#define WORKBUDDY_C_BAR_BG      0x384038
#define WORKBUDDY_C_FRAME       0xFCC430
#define WORKBUDDY_C_RECORD_OFF  0xF0C430
#define WORKBUDDY_C_RECORD_ON   0xE84D41

/* The four 64x64 screen icons, loaded from the same .bin set the carousel uses. */
#define WORKBUDDY_ICON_TASKS    "ui_img_tasks_png"
#define WORKBUDDY_ICON_POINTS   "ui_img_points_png"
#define WORKBUDDY_ICON_LEFT     "ui_img_420932938"
#define WORKBUDDY_ICON_BUTTON   "ui_img_62505970"
#define WORKBUDDY_TEXT_MAX      256
#define WORKBUDDY_REPLY_PREFIX  "回复："

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *status;        /* 等待中 — main status line                     */
    lv_obj_t *counter;       /* 5421 — count on the header strip              */
    lv_obj_t *detail;        /* 未关联 — association state                    */
    lv_obj_t *score;         /* 0 — trailing counter                          */
    lv_obj_t *battery;
    lv_obj_t *output;        /* the big body text area                        */
    lv_obj_t *record_button;
    lv_obj_t *record_label;
    bool      recording;
} workbuddy_ui_t;

static workbuddy_ui_t s_wb;
static TaskHandle_t s_audio_bridge_task;
static volatile bool s_audio_bridge_stop;
static system_ui_workbuddy_audio_tx_cb_t s_audio_tx_cb;
static void *s_audio_tx_user_ctx;

static char s_wb_reply_status[48] = WORKBUDDY_REPLY_PREFIX;
static char s_wb_reply_text[WORKBUDDY_TEXT_MAX] = "等待中";

typedef struct {
    void *enc;
    void *dec;
    uint8_t *pcm_in;
    uint8_t *opus;
    uint8_t *pcm_out;
    size_t pending;
    int enc_in_size;
    int enc_out_size;
    size_t pcm_out_size;
    bool ready;
} workbuddy_opus_bridge_t;

static SemaphoreHandle_t s_audio_playback_lock;
static workbuddy_opus_bridge_t s_audio_playback_opus;
static uint32_t s_audio_tx_seq;
static system_ui_workbuddy_audio_state_cb_t s_audio_state_cb;
/* 1.0 底座模式：首页按住说话时 Opus 交给本机 voice_cloud，不走蓝牙。 */
static esp_err_t (*volatile s_audio_local_sink)(const uint8_t *, size_t);

void system_ui_workbuddy_set_local_sink(esp_err_t (*sink)(const uint8_t *, size_t))
{
    s_audio_local_sink = sink;
}
static void *s_audio_state_user_ctx;
static volatile bool s_audio_capturing;
static volatile uint32_t s_audio_mic_gaps;

bool workbuddy_get_audio_data(audio_data_packet_t *packet, uint32_t timeout_ms)
{
    size_t size = 0;
    uint32_t seq = 0;
    lua_audio_shared_format_t fmt = {0};
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    if (packet == NULL) {
        return false;
    }

    do {
        esp_err_t err = lua_audio_shared_area_read(AUDIO_SHARED_MIC_AREA,
                                                   packet->data,
                                                   sizeof(packet->data),
                                                   &size,
                                                   &fmt,
                                                   &seq,
                                                   packet->timestamp,
                                                   false);
        if (err == ESP_OK) {
            packet->size = size;
            packet->timestamp = seq;
            packet->sample_rate = fmt.sample_rate;
            packet->channels = fmt.channels;
            packet->bits = fmt.bits;
            return true;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            return false;
        }
        if (timeout_ms == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (xTaskGetTickCount() < deadline);

    return false;
}

bool workbuddy_write_audio_data(const audio_data_packet_t *packet)
{
    lua_audio_shared_format_t fmt = {
        .sample_rate = packet && packet->sample_rate ? packet->sample_rate : 48000,
        .channels = packet && packet->channels ? packet->channels : 1,
        .bits = packet && packet->bits ? packet->bits : 16,
    };

    if (packet == NULL || packet->size == 0 || packet->size > sizeof(packet->data)) {
        return false;
    }

    return lua_audio_shared_area_write(AUDIO_SHARED_SPK_AREA,
                                       packet->data,
                                       packet->size,
                                       &fmt,
                                       NULL) == ESP_OK;
}

/*
 * SquareLine builds this screen out of lv_container/lv_textarea with every
 * style overridden back to "plain rectangle, no scroll". lv_obj + the same
 * overrides is the LVGL-v9 spelling of the same thing.
 */
static lv_obj_t *workbuddy_block(lv_obj_t *parent, int32_t w, int32_t h,
                                 int32_t x, int32_t y, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, w);
    lv_obj_set_height(obj, h);
    lv_obj_set_x(obj, x);
    lv_obj_set_y(obj, y);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (radius) {
        lv_obj_set_style_radius(obj, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    return obj;
}

static void workbuddy_fill(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* Read-only caption in the system font.
 *
 * SquareLine anchors every widget with lv_obj_set_align(..., LV_ALIGN_CENTER),
 * so the x / y in the generated file are offsets from the parent's centre. This
 * mirrors that: without the set_align call lv_obj_set_x/y fall back to
 * LV_ALIGN_DEFAULT, whose first enumerator is LV_ALIGN_TOP_LEFT, which would
 * shift every caption by half its box. */
static lv_obj_t *workbuddy_label(lv_obj_t *parent, int32_t w, int32_t h,
                                 int32_t x, int32_t y, const char *text,
                                 uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_width(label, w);
    lv_obj_set_height(label, h);
    lv_obj_set_x(label, x);
    lv_obj_set_y(label, y);
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(label);
    return label;
}

/* The main readout, mirroring ui_TextArea17 one-for-one.
 *
 * This one keeps the generated file's widget type on purpose. The text comes out
 * left-aligned with padding because the default theme styles lv_textarea that
 * way: text_align defaults to AUTO, which lv_bidi_calculate_align() resolves to
 * LEFT for LTR text. An lv_label would centre it instead, since its default
 * text_align differs - which is exactly what was wrong before.
 *
 * The cursor and selected-part styling the generated file carries is dropped:
 * that is chrome for an editor nobody types into here.
 *
 * The font stays the system one - ui_font_Font1 only carries the glyphs 会议纪要
 * and ui_font_Font2 does not exist in this tree at all. */
static lv_obj_t *workbuddy_textarea(lv_obj_t *parent, int32_t w, int32_t h,
                                    int32_t x, int32_t y, const char *text,
                                    uint32_t color)
{
    lv_obj_t *ta = lv_textarea_create(parent);

    lv_obj_set_width(ta, w);
    lv_obj_set_height(ta, h);
    lv_obj_set_x(ta, x);
    lv_obj_set_y(ta, y);
    lv_obj_set_align(ta, LV_ALIGN_CENTER);
    lv_textarea_set_text(ta, text);
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(ta, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ta, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x080C08), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    system_ui_apply_font(ta);
    return ta;
}

/* Icon loaded from the shared SD .bin set. A missing file leaves the slot empty
 * rather than aborting the screen — the carousel degrades the same way.
 *
 * ui_squareline_resolve_img() hands back one of a few rotating static buffers,
 * so the source must be set before a second call can recycle the slot. */
static void workbuddy_icon(lv_obj_t *parent, const char *name,
                           int32_t x, int32_t y, int32_t zoom)
{
    const char *path = ui_squareline_resolve_img(name);
    lv_obj_t *img;

    if (path == NULL) {
        ESP_LOGW(SYSTEM_UI_TAG, "workbuddy icon '%s' has no path, skipping", name);
        return;
    }
    img = lv_image_create(parent);
    lv_image_set_src(img, path);
    lv_obj_set_x(img, x);
    lv_obj_set_y(img, y);
    lv_obj_set_align(img, LV_ALIGN_CENTER);
    lv_image_set_scale(img, zoom);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void workbuddy_opus_bridge_deinit(workbuddy_opus_bridge_t *opus)
{
    if (opus == NULL) {
        return;
    }
    if (opus->enc != NULL) {
        esp_opus_enc_close(opus->enc);
    }
    if (opus->dec != NULL) {
        (void)esp_opus_dec_close(opus->dec);
    }
    free(opus->pcm_in);
    free(opus->opus);
    free(opus->pcm_out);
    memset(opus, 0, sizeof(*opus));
}

static esp_err_t workbuddy_opus_bridge_init(workbuddy_opus_bridge_t *opus)
{
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();

    memset(opus, 0, sizeof(*opus));

    enc_cfg.sample_rate = AUDIO_OPUS_SAMPLE_RATE;
    enc_cfg.channel = AUDIO_OPUS_CHANNELS;
    enc_cfg.bits_per_sample = AUDIO_OPUS_BITS;
    enc_cfg.bitrate = AUDIO_OPUS_BITRATE;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = 1;
    enc_cfg.enable_vbr = true;

    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus->enc) != ESP_AUDIO_ERR_OK ||
        opus->enc == NULL) {
        ESP_LOGE(SYSTEM_UI_TAG, "workbuddy opus encoder open failed");
        workbuddy_opus_bridge_deinit(opus);
        return ESP_FAIL;
    }
    if (esp_opus_enc_get_frame_size(opus->enc, &opus->enc_in_size, &opus->enc_out_size) != ESP_AUDIO_ERR_OK ||
        opus->enc_in_size <= 0 || opus->enc_out_size <= 0) {
        ESP_LOGE(SYSTEM_UI_TAG, "workbuddy opus encoder frame size failed");
        workbuddy_opus_bridge_deinit(opus);
        return ESP_FAIL;
    }
    if (opus->enc_in_size != AUDIO_OPUS_PCM_FRAME_BYTES) {
        ESP_LOGW(SYSTEM_UI_TAG, "workbuddy opus frame bytes: expected=%u actual=%d",
                 (unsigned)AUDIO_OPUS_PCM_FRAME_BYTES, opus->enc_in_size);
    }

    dec_cfg.sample_rate = AUDIO_OPUS_SAMPLE_RATE;
    dec_cfg.channel = AUDIO_OPUS_CHANNELS;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited = false;
    if (esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &opus->dec) != ESP_AUDIO_ERR_OK ||
        opus->dec == NULL) {
        ESP_LOGE(SYSTEM_UI_TAG, "workbuddy opus decoder open failed");
        workbuddy_opus_bridge_deinit(opus);
        return ESP_FAIL;
    }

    opus->pcm_out_size = AUDIO_OPUS_PCM_FRAME_BYTES * 2;
    if ((size_t)opus->enc_in_size > opus->pcm_out_size) {
        opus->pcm_out_size = (size_t)opus->enc_in_size * 2;
    }
    opus->pcm_in = malloc((size_t)opus->enc_in_size);
    opus->opus = malloc((size_t)opus->enc_out_size);
    opus->pcm_out = malloc(opus->pcm_out_size);
    if (opus->pcm_in == NULL || opus->opus == NULL || opus->pcm_out == NULL) {
        ESP_LOGE(SYSTEM_UI_TAG, "workbuddy opus buffer alloc failed");
        workbuddy_opus_bridge_deinit(opus);
        return ESP_ERR_NO_MEM;
    }

    opus->ready = true;
    ESP_LOGI(SYSTEM_UI_TAG, "workbuddy opus bridge ready: pcm_in=%d opus_max=%d",
             opus->enc_in_size, opus->enc_out_size);
    return ESP_OK;
}

static esp_err_t workbuddy_opus_bridge_process(workbuddy_opus_bridge_t *opus,
                                               const uint8_t *pcm,
                                               size_t pcm_size,
                                               size_t *encoded_bytes)
{
    esp_audio_enc_in_frame_t enc_in = {0};
    esp_audio_enc_out_frame_t enc_out = {0};

    if (encoded_bytes != NULL) {
        *encoded_bytes = 0;
    }
    if (opus == NULL || !opus->ready || pcm == NULL || encoded_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (opus->pending + pcm_size > (size_t)opus->enc_in_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(opus->pcm_in + opus->pending, pcm, pcm_size);
    opus->pending += pcm_size;
    if (opus->pending < (size_t)opus->enc_in_size) {
        return ESP_ERR_NOT_FOUND;
    }

    enc_in.buffer = opus->pcm_in;
    enc_in.len = (uint32_t)opus->enc_in_size;
    enc_out.buffer = opus->opus;
    enc_out.len = (uint32_t)opus->enc_out_size;
    if (esp_opus_enc_process(opus->enc, &enc_in, &enc_out) != ESP_AUDIO_ERR_OK ||
        enc_out.encoded_bytes == 0) {
        opus->pending = 0;
        ESP_LOGW(SYSTEM_UI_TAG, "workbuddy opus encode failed");
        return ESP_FAIL;
    }

    opus->pending = 0;
    *encoded_bytes = enc_out.encoded_bytes;
    return ESP_OK;
}

esp_err_t system_ui_workbuddy_set_audio_tx_callback(system_ui_workbuddy_audio_tx_cb_t cb,
                                                    void *user_ctx)
{
    s_audio_tx_cb = cb;
    s_audio_tx_user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t system_ui_workbuddy_play_opus(const uint8_t *opus_data, size_t opus_size)
{
    esp_audio_dec_in_raw_t dec_in = {0};
    esp_audio_dec_out_frame_t dec_out = {0};
    esp_audio_dec_info_t dec_info = {0};
    lua_audio_shared_format_t out_fmt = {
        .sample_rate = AUDIO_OPUS_SAMPLE_RATE,
        .channels = AUDIO_OPUS_CHANNELS,
        .bits = AUDIO_OPUS_BITS,
    };

    ESP_RETURN_ON_FALSE(opus_data != NULL && opus_size > 0, ESP_ERR_INVALID_ARG,
                        SYSTEM_UI_TAG, "empty opus frame");
    if (s_audio_playback_lock == NULL) {
        s_audio_playback_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_audio_playback_lock != NULL, ESP_ERR_NO_MEM,
                            SYSTEM_UI_TAG, "audio playback lock alloc failed");
    }
    if (xSemaphoreTake(s_audio_playback_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_audio_playback_opus.ready) {
        esp_err_t init_err = workbuddy_opus_bridge_init(&s_audio_playback_opus);
        if (init_err != ESP_OK) {
            xSemaphoreGive(s_audio_playback_lock);
            return init_err;
        }
    }

    dec_in.buffer = (uint8_t *)opus_data;
    dec_in.len = (uint32_t)opus_size;
    dec_out.buffer = s_audio_playback_opus.pcm_out;
    dec_out.len = (uint32_t)s_audio_playback_opus.pcm_out_size;
    esp_audio_err_t dec_err = esp_opus_dec_decode(s_audio_playback_opus.dec, &dec_in, &dec_out, &dec_info);
    if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && dec_out.needed_size > s_audio_playback_opus.pcm_out_size) {
        uint8_t *new_out = realloc(s_audio_playback_opus.pcm_out, dec_out.needed_size);
        if (new_out == NULL) {
            xSemaphoreGive(s_audio_playback_lock);
            return ESP_ERR_NO_MEM;
        }
        s_audio_playback_opus.pcm_out = new_out;
        s_audio_playback_opus.pcm_out_size = dec_out.needed_size;
        dec_in.consumed = 0;
        dec_out.buffer = s_audio_playback_opus.pcm_out;
        dec_out.len = (uint32_t)s_audio_playback_opus.pcm_out_size;
        dec_out.needed_size = 0;
        dec_out.decoded_size = 0;
        dec_err = esp_opus_dec_decode(s_audio_playback_opus.dec, &dec_in, &dec_out, &dec_info);
    }
    if (dec_err != ESP_AUDIO_ERR_OK || dec_out.decoded_size == 0) {
        xSemaphoreGive(s_audio_playback_lock);
        ESP_LOGW(SYSTEM_UI_TAG, "workbuddy incoming opus decode failed: err=%d decoded=%u",
                 dec_err, (unsigned)dec_out.decoded_size);
        return ESP_FAIL;
    }

    esp_err_t err = lua_audio_shared_area_write(AUDIO_SHARED_SPK_AREA,
                                                s_audio_playback_opus.pcm_out,
                                                dec_out.decoded_size,
                                                &out_fmt,
                                                NULL);
    xSemaphoreGive(s_audio_playback_lock);
    return err;
}

static void workbuddy_audio_bridge_task(void *arg)
{
    uint8_t *buf = malloc(AUDIO_LOOPBACK_BUFFER_SIZE);
    workbuddy_opus_bridge_t opus = {0};
    uint32_t last_mic_seq = 0;
    uint32_t loop_count = 0;

    (void)arg;
    if (buf == NULL) {
        ESP_LOGE(SYSTEM_UI_TAG, "workbuddy audio bridge alloc failed");
        s_audio_bridge_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (workbuddy_opus_bridge_init(&opus) != ESP_OK) {
        free(buf);
        s_audio_bridge_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGI(SYSTEM_UI_TAG, "workbuddy audio bridge started");
    while (!s_audio_bridge_stop) {
        size_t size = 0;
        uint32_t seq = 0;
        lua_audio_shared_format_t fmt = {0};
        esp_err_t err = lua_audio_shared_area_wait(AUDIO_SHARED_MIC_AREA,
                                                   last_mic_seq,
                                                   AUDIO_LOOPBACK_WAIT_MS);

        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "workbuddy audio bridge wait failed: %s",
                     esp_err_to_name(err));
            continue;
        }

        err = lua_audio_shared_area_read(AUDIO_SHARED_MIC_AREA,
                                         buf,
                                         AUDIO_LOOPBACK_BUFFER_SIZE,
                                         &size,
                                         &fmt,
                                         &seq,
                                         last_mic_seq,
                                         false);

        if (err == ESP_OK) {
            /* The mic area holds one chunk; a seq jump means this task was too
             * slow and chunks were overwritten before it could read them. */
            if (last_mic_seq != 0 && seq > last_mic_seq + 1) {
                s_audio_mic_gaps += seq - last_mic_seq - 1;
            }
            last_mic_seq = seq;
            if (fmt.sample_rate != AUDIO_OPUS_SAMPLE_RATE ||
                fmt.channels != AUDIO_OPUS_CHANNELS ||
                fmt.bits != AUDIO_OPUS_BITS) {
                ESP_LOGW(SYSTEM_UI_TAG,
                         "workbuddy opus requires %d/%d/%d input, got %" PRIu32 "/%u/%u",
                         AUDIO_OPUS_SAMPLE_RATE, AUDIO_OPUS_CHANNELS, AUDIO_OPUS_BITS,
                         fmt.sample_rate, (unsigned)fmt.channels, (unsigned)fmt.bits);
                continue;
            }

            size_t encoded_size = 0;
            err = workbuddy_opus_bridge_process(&opus, buf, size, &encoded_size);
            if (err == ESP_ERR_NOT_FOUND) {
                continue;
            }
            if (err == ESP_OK) {
                esp_err_t (*sink)(const uint8_t *, size_t) = s_audio_local_sink;
                system_ui_workbuddy_audio_tx_cb_t cb = s_audio_tx_cb;
                if (sink != NULL) {
                    ++s_audio_tx_seq;
                    err = sink(opus.opus, encoded_size);
                } else if (cb != NULL) {
                    err = cb(opus.opus, encoded_size, ++s_audio_tx_seq, s_audio_tx_user_ctx);
                } else {
                    err = ESP_ERR_INVALID_STATE;
                }
            }
            if (err != ESP_OK) {
                ESP_LOGW(SYSTEM_UI_TAG, "workbuddy audio bridge send failed: %s",
                         esp_err_to_name(err));
            } else if (++loop_count == 1 || (loop_count % 100) == 0) {
                ESP_LOGI(SYSTEM_UI_TAG,
                         "workbuddy Opus uplink: mic_seq=%" PRIu32 " pcm=%u opus=%u fmt=%" PRIu32 "/%u/%u",
                         seq, (unsigned)size, (unsigned)encoded_size,
                         fmt.sample_rate, (unsigned)fmt.channels, (unsigned)fmt.bits);
            }
        } else if (err == ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(SYSTEM_UI_TAG, "workbuddy mic chunk too large for bridge buffer: need=%u",
                     (unsigned)size);
            last_mic_seq = seq;
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(SYSTEM_UI_TAG, "workbuddy audio bridge read failed: %s",
                     esp_err_to_name(err));
        }
    }

    workbuddy_opus_bridge_deinit(&opus);
    free(buf);
    ESP_LOGI(SYSTEM_UI_TAG, "workbuddy audio bridge stopped");
    s_audio_bridge_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static bool workbuddy_audio_bridge_start(void)
{
    UBaseType_t task_caps = MALLOC_CAP_INTERNAL;

    s_audio_bridge_stop = false;
    if (s_audio_bridge_task != NULL) {
        return true;
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= AUDIO_LOOPBACK_TASK_STACK_SIZE) {
        task_caps = MALLOC_CAP_SPIRAM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(workbuddy_audio_bridge_task,
                                        "wb_audio_bridge",
                                        AUDIO_LOOPBACK_TASK_STACK_SIZE,
                                        NULL,
                                        AUDIO_LOOPBACK_TASK_PRIORITY,
                                        &s_audio_bridge_task,
                                        tskNO_AFFINITY,
                                        task_caps) != pdPASS) {
        s_audio_bridge_task = NULL;
        ESP_LOGE(SYSTEM_UI_TAG,
                 "start workbuddy audio bridge failed: stack=%u caps=%s internal_largest=%u psram_largest=%u",
                 (unsigned)AUDIO_LOOPBACK_TASK_STACK_SIZE,
                 task_caps == MALLOC_CAP_SPIRAM ? "PSRAM" : "INTERNAL",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return false;
    }
    ESP_LOGI(SYSTEM_UI_TAG,
             "workbuddy audio bridge task created: stack=%u caps=%s",
             (unsigned)AUDIO_LOOPBACK_TASK_STACK_SIZE,
             task_caps == MALLOC_CAP_SPIRAM ? "PSRAM" : "INTERNAL");
    return true;
}

static void workbuddy_audio_bridge_stop(void)
{
    s_audio_bridge_stop = true;
}

esp_err_t system_ui_workbuddy_set_audio_state_callback(system_ui_workbuddy_audio_state_cb_t cb,
                                                       void *user_ctx)
{
    s_audio_state_cb = cb;
    s_audio_state_user_ctx = user_ctx;
    return ESP_OK;
}

bool system_ui_workbuddy_audio_capturing(void)
{
    return s_audio_capturing;
}

uint32_t system_ui_workbuddy_audio_mic_gaps(void)
{
    return s_audio_mic_gaps;
}

static void workbuddy_audio_session_begin(bool capture_mic)
{
    if (capture_mic && !s_audio_capturing) {
        /* A fresh session numbers its packets from 1 again. */
        s_audio_tx_seq = 0;
        s_audio_mic_gaps = 0;
    }
}

static void workbuddy_audio_session_started(bool capture_mic, const char *source)
{
    ESP_LOGI(SYSTEM_UI_TAG, "workbuddy audio session started capture_mic=%d source=%s",
             capture_mic, source ? source : "device");
    if (capture_mic && !s_audio_capturing) {
        s_audio_capturing = true;
        system_ui_workbuddy_audio_state_cb_t cb = s_audio_state_cb;
        if (cb != NULL) {
            cb(true, source ? source : "device", s_audio_state_user_ctx);
        }
    }
}

esp_err_t system_ui_workbuddy_start_audio_session_from(bool capture_mic, const char *source)
{
    workbuddy_audio_session_begin(capture_mic);
    if (capture_mic && !workbuddy_audio_bridge_start()) {
        return ESP_FAIL;
    }
    ui_squareline_launch_app(AUDIO_SCRIPT_PATH);
    workbuddy_audio_session_started(capture_mic, source);
    return ESP_OK;
}

static esp_err_t workbuddy_resolve_mic_script(char *path, size_t path_size)
{
    struct stat st;

    if (claw_paths_join(CLAW_PATH_SYSTEM, AUDIO_SCRIPT_PATH, path, path_size) == ESP_OK &&
            stat(path, &st) == 0) {
        return ESP_OK;
    }
    if (claw_paths_join(CLAW_PATH_DATA, AUDIO_SCRIPT_DATA_FALLBACK, path, path_size) == ESP_OK &&
            stat(path, &st) == 0) {
        return ESP_OK;
    }
    ESP_LOGW(SYSTEM_UI_TAG, "mic script missing in SYSTEM and DATA (%s)", AUDIO_SCRIPT_DATA_FALLBACK);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t system_ui_workbuddy_start_capture_background(const char *source)
{
    char path[256];
    char output[160] = {0};
    esp_err_t err;

    if (s_audio_capturing) {
        return ESP_OK;
    }
    err = workbuddy_resolve_mic_script(path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    workbuddy_audio_session_begin(true);
    if (!workbuddy_audio_bridge_start()) {
        return ESP_FAIL;
    }
    /* Its own exclusive group: unlike the launcher ("display"), this does not
     * replace a Lua app that is on screen, so a host can listen while an app
     * keeps running. */
    err = cap_lua_run_script_async(path, NULL, 0, "voice_mic", "voice", true,
                                   output, sizeof(output));
    if (err != ESP_OK) {
        ESP_LOGE(SYSTEM_UI_TAG, "voice mic script start failed: %s %s",
                 esp_err_to_name(err), output);
        workbuddy_audio_bridge_stop();
        return err;
    }
    workbuddy_audio_session_started(true, source);
    return ESP_OK;
}

esp_err_t system_ui_workbuddy_start_audio_session(bool capture_mic)
{
    return system_ui_workbuddy_start_audio_session_from(capture_mic, "device");
}

esp_err_t system_ui_workbuddy_stop_audio_session_from(const char *reason)
{
    bool was_capturing = s_audio_capturing;

    s_audio_capturing = false;
    workbuddy_audio_bridge_stop();
    FILE *f = fopen(AUDIO_CONTROL_FILE, "w");
    if (f) {
        fprintf(f, "stop\n");
        fclose(f);
    }
    ESP_LOGI(SYSTEM_UI_TAG, "workbuddy audio session stop requested reason=%s gaps=%" PRIu32,
             reason ? reason : "device", s_audio_mic_gaps);
    if (was_capturing) {
        system_ui_workbuddy_audio_state_cb_t cb = s_audio_state_cb;
        if (cb != NULL) {
            cb(false, reason ? reason : "device", s_audio_state_user_ctx);
        }
    }
    return ESP_OK;
}

esp_err_t system_ui_workbuddy_stop_audio_session(void)
{
    return system_ui_workbuddy_stop_audio_session_from("device");
}

static bool workbuddy_truthy_value(const char *value)
{
    return value && value[0] &&
           strcmp(value, "0") != 0 &&
           strcasecmp(value, "false") != 0 &&
           strcasecmp(value, "off") != 0 &&
           strcasecmp(value, "no") != 0;
}

static void workbuddy_set_record_visual_locked(bool recording)
{
    s_wb.recording = recording;
    if (s_wb.record_label) {
        lv_label_set_text(s_wb.record_label, recording ? "停止录音" : "开始录音");
    }
    if (s_wb.record_button) {
        lv_obj_set_style_bg_color(s_wb.record_button,
                                  lv_color_hex(recording ? WORKBUDDY_C_RECORD_ON
                                                         : WORKBUDDY_C_RECORD_OFF),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void workbuddy_refresh_reply_locked(void)
{
    char text[WORKBUDDY_TEXT_MAX + sizeof(s_wb_reply_status) + 4];

    snprintf(text, sizeof(text), "%s%s", s_wb_reply_status, s_wb_reply_text);
    if (s_wb.output) {
        lv_textarea_set_text(s_wb.output, text);
    }
}

esp_err_t system_ui_update_workbuddy(const system_ui_workbuddy_update_t *update)
{
    ESP_RETURN_ON_FALSE(update != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "workbuddy update is null");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");

    if (update->tasks && s_wb.score) {
        lv_label_set_text(s_wb.score, update->tasks);
    }
    if (update->credits && s_wb.counter) {
        lv_label_set_text(s_wb.counter, update->credits);
    }
    if (update->battery && s_wb.battery) {
        int value = atoi(update->battery);
        if (value < 0) {
            value = 0;
        } else if (value > 100) {
            value = 100;
        }
        lv_bar_set_value(s_wb.battery, value, LV_ANIM_OFF);
    }
    if (update->online && s_wb.detail) {
        lv_label_set_text(s_wb.detail, workbuddy_truthy_value(update->online) ? "已关联" : "未关联");
    }
    if (update->workbuddy_status && s_wb.status) {
        lv_label_set_text(s_wb.status, update->workbuddy_status);
    } else if (update->online && s_wb.status) {
        lv_label_set_text(s_wb.status, workbuddy_truthy_value(update->online) ? "已连接" : "等待中");
    }
    if (update->record) {
        workbuddy_set_record_visual_locked(workbuddy_truthy_value(update->record));
    }
    if (update->reply_status) {
        strlcpy(s_wb_reply_status, update->reply_status, sizeof(s_wb_reply_status));
    }
    if (update->reply_text) {
        strlcpy(s_wb_reply_text, update->reply_text, sizeof(s_wb_reply_text));
    }
    if (update->reply_status || update->reply_text) {
        workbuddy_refresh_reply_locked();
    }

    system_ui_unlock();
    return ESP_OK;
}

/* Starting the mic script can wait for a previous instance to stop, so the
 * record button hands it to a short-lived task instead of blocking LVGL. */
static void workbuddy_button_capture_task(void *arg)
{
    (void)arg;
    esp_err_t err = system_ui_workbuddy_start_capture_background("button");

    if (err != ESP_OK) {
        (void)system_ui_update_workbuddy(&(system_ui_workbuddy_update_t) {
            .record = "0",
            .workbuddy_status = err == ESP_ERR_NOT_FOUND ? "缺少录音脚本" : "录音启动失败",
        });
    } else {
        ESP_LOGI(SYSTEM_UI_TAG, "WorkBuddy BLE audio uplink triggered");
    }
    vTaskDelete(NULL);
}

/* Toggle the recorder state and trigger/stop headless Lua audio script */
static void workbuddy_record_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    bool next_recording = !s_wb.recording;
    workbuddy_set_record_visual_locked(next_recording);
    lv_label_set_text(s_wb.status, s_wb.recording ? "录音中" : "等待中");

    if (s_wb.recording) {
        if (xTaskCreate(workbuddy_button_capture_task, "wb_rec_start", 4096, NULL, 4, NULL) != pdPASS) {
            workbuddy_set_record_visual_locked(false);
            lv_label_set_text(s_wb.status, "等待中");
        }
    } else {
        (void)system_ui_workbuddy_stop_audio_session_from("button");
    }
}

/* Leave the screen and put the carousel back, still on the workbuddy page. */
static void workbuddy_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    (void)system_ui_hide_workbuddy();
}

esp_err_t system_ui_create_workbuddy_locked(void)
{
    lv_obj_t *root;
    lv_obj_t *bar;
    lv_obj_t *obj;
    lv_obj_t *btn;

    if (s_wb.screen != NULL) {
        return ESP_OK;
    }

    s_wb.screen = lv_obj_create(NULL);
    ESP_RETURN_ON_FALSE(s_wb.screen != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG,
                        "create workbuddy screen failed");
    lv_obj_clear_flag(s_wb.screen, LV_OBJ_FLAG_SCROLLABLE);

    root = workbuddy_block(s_wb.screen, WORKBUDDY_ROOT_W, WORKBUDDY_ROOT_H,
                           0, WORKBUDDY_ROOT_Y, 0);
    workbuddy_fill(root, WORKBUDDY_C_BG);

    /* --- status bar with its yellow marker (ui_Container30/31) --- */
    bar = workbuddy_block(root, 480, 48, 0, -376, 0);
    workbuddy_fill(bar, WORKBUDDY_C_PANEL);
    workbuddy_fill(workbuddy_block(bar, 12, 48, -234, 0, 0), WORKBUDDY_C_ACCENT_HI);

    workbuddy_label(root, 88, 54, 140, -366, "9:51", WORKBUDDY_C_TEXT);

    /* --- battery gauge (ui_Bar4) --- */
    obj = lv_bar_create(root);
    lv_bar_set_value(obj, 70, LV_ANIM_OFF);
    lv_bar_set_start_value(obj, 0, LV_ANIM_OFF);
    lv_obj_set_width(obj, 32);
    lv_obj_set_height(obj, 9);
    lv_obj_set_x(obj, 208);
    lv_obj_set_y(obj, -376);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    workbuddy_fill(obj, WORKBUDDY_C_BAR_BG);
    lv_obj_set_style_radius(obj, 1, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(WORKBUDDY_C_ACCENT), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    s_wb.battery = obj;

    /* --- header strip with the orange dot (ui_Container32/33) --- */
    bar = workbuddy_block(root, 240, 41, 0, -296, 0);
    workbuddy_fill(bar, WORKBUDDY_C_PANEL);
    workbuddy_fill(workbuddy_block(bar, 17, 16, -88, 2, 75), WORKBUDDY_C_DOT);

    workbuddy_label(root, 135, 58, -153, -367, "LANGUAGE", WORKBUDDY_C_TEXT);

    /* --- the framed body (ui_Container35/36) --- */
    obj = workbuddy_block(root, 440, 400, 0, -50, 20);
    lv_obj_set_style_border_color(obj, lv_color_hex(WORKBUDDY_C_FRAME), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    workbuddy_fill(workbuddy_block(obj, 400, 13, 0, -182, 10), WORKBUDDY_C_ACCENT);

    /* Main readout (ui_TextArea17). Left-aligned, like the generated file. */
    s_wb.output = workbuddy_textarea(root, 400, 340, 0, -36, "等待中", WORKBUDDY_C_TEXT);

    /* --- the four readouts around the body --- */
    s_wb.status  = workbuddy_label(root, 93, 54, -28,  -285, "等待中", WORKBUDDY_C_TEXT);
    s_wb.counter = workbuddy_label(root, 93, 54, 103,  -285, "5421",   0xDCB701);
    s_wb.detail  = workbuddy_label(root, 93, 54, -118,  196, "未关联", WORKBUDDY_C_TEXT_DIM);
    s_wb.score   = workbuddy_label(root, 93, 54, 186,   196, "0",      WORKBUDDY_C_TEXT_OK);

    /* --- record button (ui_Button6 + ui_Image17 + ui_TextArea16) --- */
    btn = lv_button_create(root);
    lv_obj_set_width(btn, 210);
    lv_obj_set_height(btn, 76);
    lv_obj_set_x(btn, 0);
    lv_obj_set_y(btn, 278);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    workbuddy_fill(btn, WORKBUDDY_C_RECORD_OFF);
    lv_obj_add_event_cb(btn, workbuddy_record_cb, LV_EVENT_CLICKED, NULL);
    s_wb.record_button = btn;

    s_wb.record_label = lv_label_create(btn);
    lv_label_set_text(s_wb.record_label, "开始录音");
    lv_obj_set_style_text_color(s_wb.record_label, lv_color_hex(WORKBUDDY_C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_wb.record_label, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(s_wb.record_label);
    /* The generated screen offset the caption to make room for the mic glyph. */
    lv_obj_align(s_wb.record_label, LV_ALIGN_CENTER, 16, 0);

    workbuddy_icon(btn, WORKBUDDY_ICON_BUTTON, -73, -2, 192);

    /* --- the three decorative icons (ui_Image14/15/16) --- */
    workbuddy_icon(root, WORKBUDDY_ICON_LEFT, -180, 187, 128);
    workbuddy_icon(root, WORKBUDDY_ICON_TASKS, 129, 188, 64);
    workbuddy_icon(root, WORKBUDDY_ICON_POINTS, 41, -292, 128);

    /* --- back to the carousel --- */
    btn = lv_button_create(root);
    lv_obj_set_width(btn, 120);
    lv_obj_set_height(btn, 44);
    lv_obj_set_x(btn, -170);
    lv_obj_set_y(btn, 362);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    workbuddy_fill(btn, WORKBUDDY_C_PANEL);
    lv_obj_set_style_border_color(btn, lv_color_hex(WORKBUDDY_C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    system_ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, workbuddy_back_cb, LV_EVENT_CLICKED, NULL);

    obj = lv_label_create(btn);
    lv_label_set_text(obj, "返回");
    lv_obj_set_style_text_color(obj, lv_color_hex(WORKBUDDY_C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(obj);
    lv_obj_center(obj);

    ESP_LOGI(SYSTEM_UI_TAG, "workbuddy screen created");
    s_ui.workbuddy_screen = s_wb.screen;
    return ESP_OK;
}

void system_ui_delete_workbuddy_locked(void)
{
    if (s_audio_capturing) {
        (void)system_ui_workbuddy_stop_audio_session_from("ui_closed");
    }
    workbuddy_audio_bridge_stop();
    if (s_wb.screen != NULL) {
        lv_obj_del(s_wb.screen);
    }
    lv_memzero(&s_wb, sizeof(s_wb));
    s_ui.workbuddy_screen = NULL;
    s_ui.workbuddy_visible = false;
}

esp_err_t system_ui_show_workbuddy(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.workbuddy_screen, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "workbuddy screen not available");
    ESP_RETURN_ON_FALSE(!display_service_has_exclusive_session(), ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "exclusive display session active");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    s_ui.workbuddy_visible = true;
    system_ui_load_screen_locked(s_ui.workbuddy_screen);
    system_ui_unlock();
    return ESP_OK;
}

esp_err_t system_ui_hide_workbuddy(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.home_screen, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "workbuddy screen not available");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    s_ui.workbuddy_visible = false;
    /* Land back on the page that opened it, so the carousel does not jump. */
    ui_squareline_show_page(ui_squareline_current_page());
    system_ui_load_screen_locked(s_ui.home_screen);
    system_ui_unlock();
    return ESP_OK;
}

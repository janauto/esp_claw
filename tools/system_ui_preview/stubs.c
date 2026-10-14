/* Host stand-ins for the firmware services the screens touch. */
#include <stddef.h>
#include "lvgl.h"
#include "system_ui_private.h"

const lv_image_dsc_t *preview_find_image(const char *name);

system_ui_state_t s_ui;

void ui_img_sd_set_base_path(const char *base_path)
{
    (void)base_path;
}

/* lv_image_set_src() tells a descriptor from a path by its first byte, so the
 * compiled-in descriptor can stand in for the .bin path. */
const char *ui_img_sd_path(const char *name)
{
    return name ? (const char *)preview_find_image(name) : NULL;
}

/* ---- Everything below only has to link: the preview never reaches audio,
 * IR, haptics or the task scheduler, it just builds the screens. ---- */
#include "display_service.h"
#include "driver/gpio.h"
#include "esp_dsp.h"
#include "esp_rom_sys.h"
#include "freertos/idf_additions.h"
#include "lua_module_audio.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"

const char *esp_err_to_name(esp_err_t err) { (void)err; return "ERR"; }

esp_err_t system_ui_lock(void) { return ESP_OK; }
void system_ui_unlock(void) {}
void system_ui_load_screen_locked(lv_obj_t *screen) { lv_screen_load(screen); }
void system_ui_add_click_feedback(lv_obj_t *obj) { (void)obj; }
bool display_service_has_exclusive_session(void) { return false; }

esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) { (void)gpio; (void)level; return ESP_OK; }
void esp_rom_delay_us(uint32_t us) { (void)us; }

esp_err_t dsps_fft2r_init_fc32(float *t, int n) { (void)t; (void)n; return ESP_OK; }
esp_err_t dsps_fft2r_fc32(float *d, int n) { (void)d; (void)n; return ESP_OK; }
esp_err_t dsps_bit_rev_fc32(float *d, int n) { (void)d; (void)n; return ESP_OK; }
void dsps_wind_hann_f32(float *w, int n) { for (int i = 0; i < n; i++) w[i] = 1.0f; }

esp_err_t lua_audio_shared_area_write(const char *name, const void *data, size_t size,
                                      const lua_audio_shared_format_t *format, uint32_t *seq_out)
{
    (void)name; (void)data; (void)size; (void)format; (void)seq_out;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t lua_audio_shared_area_read(const char *name, void *data, size_t capacity, size_t *size_out,
                                     lua_audio_shared_format_t *format_out, uint32_t *seq_out,
                                     uint32_t after_seq, bool clear_after_read)
{
    (void)name; (void)data; (void)capacity; (void)size_out; (void)format_out;
    (void)seq_out; (void)after_seq; (void)clear_after_read;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t lua_audio_shared_area_wait(const char *name, uint32_t after_seq, uint32_t timeout_ms)
{
    (void)name; (void)after_seq; (void)timeout_ms;
    return ESP_ERR_TIMEOUT;
}

esp_audio_err_t esp_opus_enc_open(void *cfg, uint32_t sz, void **h) { (void)cfg; (void)sz; (void)h; return -1; }
esp_audio_err_t esp_opus_enc_get_frame_size(void *h, int *i, int *o) { (void)h; (void)i; (void)o; return -1; }
esp_audio_err_t esp_opus_enc_process(void *h, esp_audio_enc_in_frame_t *i, esp_audio_enc_out_frame_t *o) { (void)h; (void)i; (void)o; return -1; }
void esp_opus_enc_close(void *h) { (void)h; }
esp_audio_err_t esp_opus_dec_open(void *cfg, uint32_t sz, void **h) { (void)cfg; (void)sz; (void)h; return -1; }
esp_audio_err_t esp_opus_dec_decode(void *h, esp_audio_dec_in_raw_t *r, esp_audio_dec_out_frame_t *f, esp_audio_dec_info_t *i) { (void)h; (void)r; (void)f; (void)i; return -1; }
esp_audio_err_t esp_opus_dec_close(void *h) { (void)h; return 0; }

/* No scheduler on the host: tasks are never started, locks always succeed. */
struct sim_semaphore { int unused; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { static struct sim_semaphore s; return &s; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t st, void *a, UBaseType_t p, TaskHandle_t *o)
{
    (void)f; (void)n; (void)st; (void)a; (void)p;
    if (o) *o = NULL;
    return pdFALSE;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t f, const char *n, uint32_t st, void *a,
                                           UBaseType_t p, TaskHandle_t *o, BaseType_t c, UBaseType_t caps)
{
    (void)c; (void)caps;
    return xTaskCreate(f, n, st, a, p, o);
}
void vTaskDelay(TickType_t t) { (void)t; }
void vTaskDelete(TaskHandle_t t) { (void)t; }
void vTaskDeleteWithCaps(TaskHandle_t t) { (void)t; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)lv_tick_get(); }

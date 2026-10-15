#include "claw_paths.h"
#include "display_service.h"
#include "mbedtls/base64.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static char root[512], job_path[512];
static bool exclusive;
static int locks, refreshes;
void app_test_set_root(const char *p)
{
    snprintf(root, sizeof(root), "%s", p);
    exclusive = false;
    locks = refreshes = 0;
    job_path[0] = 0;
}
void app_test_set_exclusive(int v)
{
    exclusive = v != 0;
}
void app_test_set_job(const char *p)
{
    snprintf(job_path, sizeof(job_path), "%s", p);
}
int app_test_locks(void)
{
    return locks;
}
int app_test_refreshes(void)
{
    return refreshes;
}
const char *claw_paths_get(claw_path_root_t r)
{
    (void)r;
    return root;
}
esp_err_t claw_paths_join(claw_path_root_t r, const char *rel, char *p, size_t cap)
{
    (void)r;
    int n = snprintf(p, cap, "%s/%s", root, rel);
    return n > 0 && (size_t)n < cap ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
bool display_service_is_started(void)
{
    return true;
}
bool display_service_has_exclusive_session(void)
{
    return exclusive;
}
esp_err_t display_service_lock(void)
{
    locks++;
    return ESP_OK;
}
void display_service_unlock(void)
{
    locks--;
}
esp_err_t system_ui_schedule_home_reload(void)
{
    refreshes++;
    return ESP_OK;
}
esp_err_t cap_lua_list_jobs(const char *status, char *out, size_t size)
{
    (void)status;
    snprintf(out, size, "path=%s\n", job_path);
    return ESP_OK;
}
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *size, const unsigned char *in, size_t n)
{
    size_t need = 4 * ((n + 2) / 3);
    if (cap < need + 1)
        return -1;
    *size = (size_t)EVP_EncodeBlock(out, in, (int)n);
    return 0;
}
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *size, const unsigned char *in, size_t n)
{
    if (n % 4 || n > 4 * ((cap + 2) / 3))
        return -1;
    unsigned char *temp = malloc(n + 1);
    if (!temp)
        return -1;
    int v = EVP_DecodeBlock(temp, in, (int)n);
    if (v >= 0) {
        if (n && in[n - 1] == '=')
            v--;
        if (n > 1 && in[n - 2] == '=')
            v--;
    }
    if (v < 0 || (size_t)v > cap) {
        free(temp);
        return -1;
    }
    memcpy(out, temp, (size_t)v);
    *size = (size_t)v;
    free(temp);
    return 0;
}

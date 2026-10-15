/*
 * 宿主测试桩的实现：FreeRTOS 互斥量/任务用 pthread，NVS 用进程内存里的键值表（不落盘），
 * heap_caps 落到 malloc，esp_timer 用单调时钟。只用于 test_host 与 tools，不进固件。
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

/* ---------------- esp_err ---------------- */

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: return "ESP_ERR_UNKNOWN";
    }
}

/* ---------------- heap_caps / esp_timer ---------------- */

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    (void)caps;
    return realloc(ptr, size);
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return (size_t)1 << 30;
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---------------- FreeRTOS ---------------- */

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (m) {
        pthread_mutex_init(m, NULL);
    }
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)ticks;
    return pthread_mutex_lock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    return pthread_mutex_unlock((pthread_mutex_t *)s) == 0 ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    if (s) {
        pthread_mutex_destroy((pthread_mutex_t *)s);
        free(s);
    }
}

void vTaskDelay(TickType_t ticks)
{
    usleep((useconds_t)ticks * 1000); /* 1 tick = 1 ms，与固件 CONFIG_FREERTOS_HZ=1000 一致 */
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 0;
}

static pthread_mutex_t s_fail_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_task_fail;

void host_task_fail_next(int n)
{
    pthread_mutex_lock(&s_fail_mu);
    s_task_fail = n;
    pthread_mutex_unlock(&s_fail_mu);
}

typedef struct {
    TaskFunction_t fn;
    void *arg;
} host_task_t;

static void *task_trampoline(void *p)
{
    host_task_t t = *(host_task_t *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps)
{
    (void)name;
    (void)stack;
    (void)prio;
    (void)core;
    (void)caps;
    pthread_mutex_lock(&s_fail_mu);
    bool fail = s_task_fail > 0;
    if (fail) {
        s_task_fail--;
    }
    pthread_mutex_unlock(&s_fail_mu);
    if (fail) {
        return pdFAIL;
    }
    host_task_t *t = malloc(sizeof(*t));
    if (!t) {
        return pdFAIL;
    }
    t->fn = fn;
    t->arg = arg;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int r = pthread_create(&th, &attr, task_trampoline, t);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        free(t);
        return pdFAIL;
    }
    if (out) {
        *out = (TaskHandle_t)(uintptr_t)1;
    }
    return pdPASS;
}

void vTaskDeleteWithCaps(TaskHandle_t task)
{
    if (task == NULL) {
        pthread_exit(NULL);
    }
}

/* ---------------- NVS（内存表） ---------------- */

typedef struct host_nvs_entry {
    char ns[16];
    char key[16];
    char *value;
    struct host_nvs_entry *next;
} host_nvs_entry_t;

static pthread_mutex_t s_nvs_mu = PTHREAD_MUTEX_INITIALIZER;
static host_nvs_entry_t *s_nvs;
static char s_ns[8][16];
static int s_ns_count;
static int s_nvs_fail_set;

static int ns_index(const char *ns)
{
    for (int i = 0; i < s_ns_count; i++) {
        if (strcmp(s_ns[i], ns) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!name || strlen(name) >= 16 || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_nvs_mu);
    int i = ns_index(name);
    if (i < 0) {
        if (mode == NVS_READONLY || s_ns_count == 8) {
            pthread_mutex_unlock(&s_nvs_mu);
            return ESP_ERR_NVS_NOT_FOUND;
        }
        i = s_ns_count++;
        strcpy(s_ns[i], name);
    }
    pthread_mutex_unlock(&s_nvs_mu);
    *out = (nvs_handle_t)(i + 1);
    return ESP_OK;
}

static host_nvs_entry_t *find(int ns, const char *key)
{
    for (host_nvs_entry_t *e = s_nvs; e; e = e->next) {
        if (strcmp(e->ns, s_ns[ns]) == 0 && strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    pthread_mutex_lock(&s_nvs_mu);
    host_nvs_entry_t *e = find((int)h - 1, key);
    esp_err_t r = ESP_OK;
    if (!e) {
        r = ESP_ERR_NVS_NOT_FOUND;
    } else {
        size_t need = strlen(e->value) + 1;
        if (out && *len < need) {
            r = ESP_ERR_NVS_INVALID_LENGTH;
        } else if (out) {
            memcpy(out, e->value, need);
        }
        *len = need;
    }
    pthread_mutex_unlock(&s_nvs_mu);
    return r;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    if (strlen(key) >= 16) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_nvs_mu);
    if (s_nvs_fail_set > 0) {
        s_nvs_fail_set--;
        pthread_mutex_unlock(&s_nvs_mu);
        return ESP_FAIL;
    }
    host_nvs_entry_t *e = find((int)h - 1, key);
    if (!e) {
        e = calloc(1, sizeof(*e));
        strcpy(e->ns, s_ns[(int)h - 1]);
        strcpy(e->key, key);
        e->next = s_nvs;
        s_nvs = e;
    }
    free(e->value);
    e->value = strdup(value);
    pthread_mutex_unlock(&s_nvs_mu);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    pthread_mutex_lock(&s_nvs_mu);
    host_nvs_entry_t **pp = &s_nvs;
    esp_err_t r = ESP_ERR_NVS_NOT_FOUND;
    while (*pp) {
        host_nvs_entry_t *e = *pp;
        if (strcmp(e->ns, s_ns[(int)h - 1]) == 0 && strcmp(e->key, key) == 0) {
            *pp = e->next;
            free(e->value);
            free(e);
            r = ESP_OK;
            break;
        }
        pp = &e->next;
    }
    pthread_mutex_unlock(&s_nvs_mu);
    return r;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    (void)h;
}

void host_nvs_clear(void)
{
    pthread_mutex_lock(&s_nvs_mu);
    while (s_nvs) {
        host_nvs_entry_t *e = s_nvs;
        s_nvs = e->next;
        free(e->value);
        free(e);
    }
    s_ns_count = 0;
    pthread_mutex_unlock(&s_nvs_mu);
}

int host_nvs_count(const char *ns)
{
    int n = 0;
    pthread_mutex_lock(&s_nvs_mu);
    for (host_nvs_entry_t *e = s_nvs; e; e = e->next) {
        if (strcmp(e->ns, ns) == 0) {
            n++;
        }
    }
    pthread_mutex_unlock(&s_nvs_mu);
    return n;
}

const char *host_nvs_peek(const char *ns, const char *key)
{
    const char *v = NULL;
    pthread_mutex_lock(&s_nvs_mu);
    for (host_nvs_entry_t *e = s_nvs; e; e = e->next) {
        if (strcmp(e->ns, ns) == 0 && strcmp(e->key, key) == 0) {
            v = e->value;
        }
    }
    pthread_mutex_unlock(&s_nvs_mu);
    return v;
}

void host_nvs_fail_next_set(int n)
{
    pthread_mutex_lock(&s_nvs_mu);
    s_nvs_fail_set = n;
    pthread_mutex_unlock(&s_nvs_mu);
}

#pragma once
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_FALSE(cond, err, tag, fmt, ...) do { \
    (void)(tag); \
    if (!(cond)) return (err); \
} while (0)

#define ESP_RETURN_ON_ERROR(expr, tag, fmt, ...) do { \
    (void)(tag); \
    esp_err_t _err = (expr); \
    if (_err != ESP_OK) return _err; \
} while (0)

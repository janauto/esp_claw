#pragma once
#include <stdbool.h>
#include "esp_err.h"
bool display_service_is_started(void);
bool display_service_has_exclusive_session(void);
esp_err_t display_service_lock(void);
void display_service_unlock(void);

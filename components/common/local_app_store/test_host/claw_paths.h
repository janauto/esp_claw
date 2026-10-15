#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef enum { CLAW_PATH_DATA, CLAW_PATH_SYSTEM } claw_path_root_t;
const char *claw_paths_get(claw_path_root_t);
esp_err_t claw_paths_join(claw_path_root_t, const char *, char *, size_t);

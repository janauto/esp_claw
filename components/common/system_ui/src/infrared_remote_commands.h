#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    IR_REMOTE_BRAND_GREE = 0,
    IR_REMOTE_BRAND_MIDEA,
    IR_REMOTE_BRAND_HAIER,
    IR_REMOTE_BRAND_MAX,
} infrared_remote_brand_t;

typedef enum {
    IR_REMOTE_MODE_COOL = 0,
    IR_REMOTE_MODE_HEAT,
    IR_REMOTE_MODE_DRY,
    IR_REMOTE_MODE_FAN,
    IR_REMOTE_MODE_SLEEP,
    IR_REMOTE_MODE_TURBO,
    IR_REMOTE_MODE_LIGHT,
    IR_REMOTE_MODE_VENT,
    IR_REMOTE_MODE_ECO,
    IR_REMOTE_MODE_SWING_LR,
    IR_REMOTE_MODE_WIFI,
    IR_REMOTE_MODE_MUTE,
    IR_REMOTE_MODE_MAX,
} infrared_remote_mode_t;

typedef enum {
    IR_REMOTE_FAN_0 = 0,
    IR_REMOTE_FAN_1,
    IR_REMOTE_FAN_2,
    IR_REMOTE_FAN_3,
    IR_REMOTE_FAN_AUTO,
    IR_REMOTE_FAN_MAX,
} infrared_remote_fan_t;

typedef enum {
    IR_REMOTE_WIND_UP = 0,
    IR_REMOTE_WIND_DOWN,
    IR_REMOTE_WIND_AUTO,
    IR_REMOTE_WIND_AUTO_STOP,
    IR_REMOTE_WIND_SWING_LR,
    IR_REMOTE_WIND_MAX,
} infrared_remote_wind_t;

typedef enum {
    IR_REMOTE_AIR_FAN = 0,
    IR_REMOTE_AIR_WIND,
} infrared_remote_air_control_t;

typedef enum {
    IR_REMOTE_ACTION_STATE = 0,
    IR_REMOTE_ACTION_POWER,
    IR_REMOTE_ACTION_MODE,
    IR_REMOTE_ACTION_TEMP,
    IR_REMOTE_ACTION_TEMP_UP,
    IR_REMOTE_ACTION_TEMP_DOWN,
    IR_REMOTE_ACTION_FAN,
    IR_REMOTE_ACTION_WIND,
    IR_REMOTE_ACTION_TIMER,
} infrared_remote_action_t;

typedef struct {
    infrared_remote_brand_t brand;
    bool power_on;
    infrared_remote_mode_t mode;
    uint8_t temperature;
    infrared_remote_fan_t fan;
    infrared_remote_wind_t wind;
} infrared_remote_state_t;

typedef struct {
    uint8_t addr;
    uint8_t iaddr;
    uint8_t cmd;
    uint8_t icmd;
    uint32_t raw;
} infrared_remote_nec_frame_t;

const char *infrared_remote_brand_name(infrared_remote_brand_t brand);
const char *infrared_remote_brand_options(void);
size_t infrared_remote_brand_count(void);

bool infrared_remote_command_build_nec(const infrared_remote_state_t *state,
                                       infrared_remote_action_t action,
                                       infrared_remote_nec_frame_t *out_frame);

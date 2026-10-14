#include "infrared_remote_commands.h"

#define IR_REMOTE_GREE_NEC_ADDR 0xFF

typedef bool (*infrared_remote_build_nec_fn_t)(const infrared_remote_state_t *state,
                                               infrared_remote_action_t action,
                                               infrared_remote_nec_frame_t *out_frame);

typedef struct {
    infrared_remote_build_nec_fn_t build_nec;
} infrared_remote_protocol_ops_t;

typedef struct {
    const char *name;
    const infrared_remote_protocol_ops_t *ops;
} infrared_remote_brand_profile_t;

typedef enum {
    IR_REMOTE_GREE_CMD_POWER = 0x00,
    IR_REMOTE_GREE_CMD_MODE = 0x01,
    IR_REMOTE_GREE_CMD_FAN = 0x02,
    IR_REMOTE_GREE_CMD_SWING = 0x03,
    IR_REMOTE_GREE_CMD_TEMP_UP = 0x04,
    IR_REMOTE_GREE_CMD_TEMP_DOWN = 0x05,
    IR_REMOTE_GREE_CMD_TIMER = 0x06,
    IR_REMOTE_GREE_CMD_SLEEP = 0x07,
    IR_REMOTE_GREE_CMD_TURBO = 0x08,
    IR_REMOTE_GREE_CMD_LIGHT = 0x09,
    IR_REMOTE_GREE_CMD_ECO = 0x0A,
    IR_REMOTE_GREE_CMD_HEALTH = 0x0B,
} infrared_remote_gree_command_t;

static bool infrared_remote_gree_build_nec(const infrared_remote_state_t *state,
                                           infrared_remote_action_t action,
                                           infrared_remote_nec_frame_t *out_frame);

static const infrared_remote_protocol_ops_t s_gree_ops = {
    .build_nec = infrared_remote_gree_build_nec,
};

static const infrared_remote_brand_profile_t s_brand_profiles[IR_REMOTE_BRAND_MAX] = {
    [IR_REMOTE_BRAND_GREE] = {
        .name = "格力",
        .ops = &s_gree_ops,
    },
    [IR_REMOTE_BRAND_MIDEA] = {
        .name = "美的",
        .ops = &s_gree_ops,
    },
    [IR_REMOTE_BRAND_HAIER] = {
        .name = "海尔",
        .ops = &s_gree_ops,
    },
};

const char *infrared_remote_brand_name(infrared_remote_brand_t brand)
{
    if (brand >= IR_REMOTE_BRAND_MAX) {
        return s_brand_profiles[IR_REMOTE_BRAND_GREE].name;
    }
    return s_brand_profiles[brand].name;
}

const char *infrared_remote_brand_options(void)
{
    return "格力\n美的\n海尔";
}

size_t infrared_remote_brand_count(void)
{
    return IR_REMOTE_BRAND_MAX;
}

static uint32_t infrared_remote_nec_pack(uint8_t addr, uint8_t cmd)
{
    return ((uint32_t)(uint8_t)~cmd << 24) |
           ((uint32_t)cmd << 16) |
           ((uint32_t)(uint8_t)~addr << 8) |
           (uint32_t)addr;
}

static void infrared_remote_fill_nec_frame(uint8_t addr, uint8_t cmd,
                                           infrared_remote_nec_frame_t *out_frame)
{
    out_frame->addr = addr;
    out_frame->iaddr = (uint8_t)~addr;
    out_frame->cmd = cmd;
    out_frame->icmd = (uint8_t)~cmd;
    out_frame->raw = infrared_remote_nec_pack(addr, cmd);
}

static uint8_t infrared_remote_gree_mode_command(infrared_remote_mode_t mode)
{
    switch (mode) {
    case IR_REMOTE_MODE_SLEEP:
        return IR_REMOTE_GREE_CMD_SLEEP;
    case IR_REMOTE_MODE_TURBO:
        return IR_REMOTE_GREE_CMD_TURBO;
    case IR_REMOTE_MODE_LIGHT:
        return IR_REMOTE_GREE_CMD_LIGHT;
    case IR_REMOTE_MODE_ECO:
        return IR_REMOTE_GREE_CMD_ECO;
    case IR_REMOTE_MODE_SWING_LR:
        return IR_REMOTE_GREE_CMD_SWING;
    case IR_REMOTE_MODE_VENT:
        return IR_REMOTE_GREE_CMD_HEALTH;
    case IR_REMOTE_MODE_WIFI:
    case IR_REMOTE_MODE_MUTE:
    case IR_REMOTE_MODE_COOL:
    case IR_REMOTE_MODE_HEAT:
    case IR_REMOTE_MODE_DRY:
    case IR_REMOTE_MODE_FAN:
    default:
        return IR_REMOTE_GREE_CMD_MODE;
    }
}

static bool infrared_remote_gree_build_nec(const infrared_remote_state_t *state,
                                           infrared_remote_action_t action,
                                           infrared_remote_nec_frame_t *out_frame)
{
    uint8_t cmd = IR_REMOTE_GREE_CMD_MODE;

    switch (action) {
    case IR_REMOTE_ACTION_POWER:
        cmd = IR_REMOTE_GREE_CMD_POWER;
        break;
    case IR_REMOTE_ACTION_MODE:
        cmd = infrared_remote_gree_mode_command(state->mode);
        break;
    case IR_REMOTE_ACTION_TEMP_UP:
        cmd = IR_REMOTE_GREE_CMD_TEMP_UP;
        break;
    case IR_REMOTE_ACTION_TEMP_DOWN:
        cmd = IR_REMOTE_GREE_CMD_TEMP_DOWN;
        break;
    case IR_REMOTE_ACTION_FAN:
        cmd = IR_REMOTE_GREE_CMD_FAN;
        break;
    case IR_REMOTE_ACTION_WIND:
        cmd = IR_REMOTE_GREE_CMD_SWING;
        break;
    case IR_REMOTE_ACTION_TIMER:
        cmd = IR_REMOTE_GREE_CMD_TIMER;
        break;
    case IR_REMOTE_ACTION_STATE:
    case IR_REMOTE_ACTION_TEMP:
    default:
        cmd = infrared_remote_gree_mode_command(state->mode);
        break;
    }

    (void)state->temperature;
    (void)state->power_on;
    (void)state->fan;
    (void)state->wind;

    infrared_remote_fill_nec_frame(IR_REMOTE_GREE_NEC_ADDR, cmd, out_frame);
    return true;
}

bool infrared_remote_command_build_nec(const infrared_remote_state_t *state,
                                       infrared_remote_action_t action,
                                       infrared_remote_nec_frame_t *out_frame)
{
    if (state == NULL || out_frame == NULL || state->brand >= IR_REMOTE_BRAND_MAX ||
        state->mode >= IR_REMOTE_MODE_MAX || state->fan >= IR_REMOTE_FAN_MAX ||
        state->wind >= IR_REMOTE_WIND_MAX) {
        return false;
    }

    const infrared_remote_brand_profile_t *profile = &s_brand_profiles[state->brand];
    if (profile->ops == NULL || profile->ops->build_nec == NULL) {
        return false;
    }

    return profile->ops->build_nec(state, action, out_frame);
}

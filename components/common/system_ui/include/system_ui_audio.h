/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio data packet for BLE transmission */
typedef struct {
    uint8_t data[4096];  /* Enough for 10ms stereo PCM @ 48kHz/16bit */
    size_t size;
    uint32_t timestamp;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
} audio_data_packet_t;

/**
 * @brief Get audio data from queue (for BLE transmission)
 *
 * @param packet Pointer to audio data packet structure
 * @param timeout_ms Timeout in milliseconds
 * @return true if data retrieved successfully, false otherwise
 */
bool workbuddy_get_audio_data(audio_data_packet_t *packet, uint32_t timeout_ms);

/**
 * @brief Put audio data into WorkBuddy speaker playback area
 *
 * @param packet Pointer to audio data packet structure
 * @return true if data was written successfully, false otherwise
 */
bool workbuddy_write_audio_data(const audio_data_packet_t *packet);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * Binary framing for the BLE voice uplink (audio characteristic 7b7d1005-...).
 *
 * One Opus packet becomes one or more notifications. Every notification starts
 * with the same 8-byte little-endian header:
 *
 *   [0] magic   0x56 ('V')
 *   [1] version 1
 *   [2] flags   bit0 FIRST fragment of this packet, bit1 LAST fragment
 *   [3] session id (wraps at 255, never 0)
 *   [4..5] packet seq, starts at 1 for every session
 *   [6..7] total Opus packet length in bytes
 *
 * Fragments of a packet are sent back to back on one link, so notification
 * order is enough to reassemble them; the receiver concatenates payloads from
 * FIRST to LAST and checks the result against the total length.
 *
 * Kept free of ESP-IDF headers so the host test can compile it directly.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_FRAME_MAGIC       0x56
#define VOICE_FRAME_VERSION     1
#define VOICE_FRAME_HEADER_LEN  8
#define VOICE_FRAME_FLAG_FIRST  0x01
#define VOICE_FRAME_FLAG_LAST   0x02

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;
    uint16_t seq;
    uint8_t session;
    uint8_t started;
} voice_frame_cursor_t;

void voice_frame_cursor_init(voice_frame_cursor_t *cursor, uint8_t session, uint16_t seq,
                             const uint8_t *data, size_t len);

/*
 * Writes the next notification of the packet into out. Returns the number of
 * bytes written, or 0 once the whole packet has been emitted or when out_cap
 * cannot hold a header plus at least one payload byte.
 */
size_t voice_frame_next(voice_frame_cursor_t *cursor, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

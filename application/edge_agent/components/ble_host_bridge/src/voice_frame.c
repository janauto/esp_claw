/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "voice_frame.h"

#include <string.h>

void voice_frame_cursor_init(voice_frame_cursor_t *cursor, uint8_t session, uint16_t seq,
                             const uint8_t *data, size_t len)
{
    if (cursor == NULL) {
        return;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->data = data;
    /* The header carries the length in 16 bits; an Opus packet is far below. */
    cursor->len = len > 0xFFFF ? 0xFFFF : len;
    cursor->session = session;
    cursor->seq = seq;
}

size_t voice_frame_next(voice_frame_cursor_t *cursor, uint8_t *out, size_t out_cap)
{
    size_t remaining;
    size_t chunk;
    uint8_t flags = 0;

    if (cursor == NULL || out == NULL || cursor->data == NULL || cursor->len == 0 ||
            out_cap <= VOICE_FRAME_HEADER_LEN) {
        return 0;
    }
    if (cursor->started && cursor->offset >= cursor->len) {
        return 0;
    }

    remaining = cursor->len - cursor->offset;
    chunk = out_cap - VOICE_FRAME_HEADER_LEN;
    if (chunk > remaining) {
        chunk = remaining;
    }
    if (!cursor->started) {
        flags |= VOICE_FRAME_FLAG_FIRST;
    }
    if (cursor->offset + chunk >= cursor->len) {
        flags |= VOICE_FRAME_FLAG_LAST;
    }

    out[0] = VOICE_FRAME_MAGIC;
    out[1] = VOICE_FRAME_VERSION;
    out[2] = flags;
    out[3] = cursor->session;
    out[4] = (uint8_t)(cursor->seq & 0xFF);
    out[5] = (uint8_t)(cursor->seq >> 8);
    out[6] = (uint8_t)(cursor->len & 0xFF);
    out[7] = (uint8_t)(cursor->len >> 8);
    memcpy(out + VOICE_FRAME_HEADER_LEN, cursor->data + cursor->offset, chunk);

    cursor->offset += chunk;
    cursor->started = 1;
    return VOICE_FRAME_HEADER_LEN + chunk;
}

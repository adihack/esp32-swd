/* SPDX-License-Identifier: MIT */
#include "esw/proto.h"

uint16_t esw_crc16(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFu;
    while (n--) {
        crc ^= (uint32_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) : (crc << 1);
        crc &= 0xFFFFu;
    }
    return (uint16_t)crc;
}

size_t esw_cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            if (++code == 0xFF) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    return o;
}

int esw_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t code = in[i++];
        if (code == 0)
            return -1;
        for (uint8_t k = 1; k < code; k++) {
            if (i >= n || o >= cap || in[i] == 0)
                return -1;
            out[o++] = in[i++];
        }
        if (code != 0xFF && i < n) {
            if (o >= cap)
                return -1;
            out[o++] = 0;
        }
    }
    return (int)o;
}

size_t esw_frame_encode(const uint8_t *payload, size_t n, uint8_t *out, size_t cap)
{
    uint8_t tmp[ESW_PROTO_MAX_PAYLOAD + 2];
    if (n > ESW_PROTO_MAX_PAYLOAD || cap < ESW_COBS_MAX(n + 2) + 1)
        return 0;
    for (size_t i = 0; i < n; i++)
        tmp[i] = payload[i];
    uint16_t crc = esw_crc16(payload, n);
    tmp[n] = (uint8_t)(crc & 0xFFu);
    tmp[n + 1] = (uint8_t)(crc >> 8);
    size_t len = esw_cobs_encode(tmp, n + 2, out);
    out[len++] = 0;
    return len;
}

void esw_frame_rx_init(esw_frame_rx_t *rx)
{
    rx->len = 0;
    rx->overflow = 0;
    rx->bad_frames = 0;
}

int esw_frame_rx_byte(esw_frame_rx_t *rx, uint8_t b, uint8_t *payload, size_t cap)
{
    if (b != 0) {
        if (rx->len < sizeof(rx->raw))
            rx->raw[rx->len++] = b;
        else
            rx->overflow = 1;
        return 0;
    }
    size_t len = rx->len;
    uint8_t ovf = rx->overflow;
    rx->len = 0;
    rx->overflow = 0;
    if (len == 0)
        return 0;   /* back-to-back delimiters */
    uint8_t tmp[ESW_PROTO_MAX_PAYLOAD + 2];
    int n = ovf ? -1 : esw_cobs_decode(rx->raw, len, tmp, sizeof(tmp));
    if (n < 3 || (size_t)(n - 2) > cap) {
        rx->bad_frames++;
        return 0;
    }
    size_t pl = (size_t)n - 2;
    uint16_t crc = (uint16_t)((uint32_t)tmp[pl] | ((uint32_t)tmp[pl + 1] << 8));
    if (crc != esw_crc16(tmp, pl)) {
        rx->bad_frames++;
        return 0;
    }
    for (size_t i = 0; i < pl; i++)
        payload[i] = tmp[i];
    return (int)pl;
}

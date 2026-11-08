/* SPDX-License-Identifier: MIT */
/* Framing: COBS round trips (zero runs, 254-byte blocks), CRC, resync after garbage. */
#include "tinytest.h"
#include <string.h>
#include "esw/proto.h"

static uint8_t frame[ESW_FRAME_MAX], out[ESW_PROTO_MAX_PAYLOAD];

static void crc_known_value(void)
{
    CHECK_EQ(esw_crc16((const uint8_t *)"123456789", 9), 0x29B1u);   /* CRC-16/CCITT-FALSE check value */
}

static int roundtrip(const uint8_t *p, size_t n)
{
    esw_frame_rx_t rx;
    esw_frame_rx_init(&rx);
    size_t fl = esw_frame_encode(p, n, frame, sizeof(frame));
    if (!fl || frame[fl - 1] != 0)
        return -1;
    for (size_t i = 0; i + 1 < fl; i++)
        if (frame[i] == 0)
            return -2;   /* COBS must remove every zero */
    int got = 0;
    for (size_t i = 0; i < fl; i++)
        got = esw_frame_rx_byte(&rx, frame[i], out, sizeof(out));
    return (got == (int)n && memcmp(out, p, n) == 0) ? 0 : -3;
}

static void cobs_roundtrips(void)
{
    static uint8_t p[ESW_PROTO_MAX_PAYLOAD];
    size_t sizes[] = { 3, 4, 253, 254, 255, 256, 508, 1000, 2048, ESW_PROTO_MAX_PAYLOAD };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (size_t i = 0; i < sizes[s]; i++) p[i] = (uint8_t)(i * 7u + 1u);        /* no zeros */
        CHECK_EQ(roundtrip(p, sizes[s]), 0);
        memset(p, 0, sizes[s]);                                                      /* all zeros */
        CHECK_EQ(roundtrip(p, sizes[s]), 0);
        for (size_t i = 0; i < sizes[s]; i++) p[i] = (uint8_t)((i % 5u) ? i : 0u);  /* mixed */
        CHECK_EQ(roundtrip(p, sizes[s]), 0);
    }
}

static void resync_after_garbage_and_bad_crc(void)
{
    esw_frame_rx_t rx;
    esw_frame_rx_init(&rx);
    const char *boot = "ESP-ROM:esp32s3-20210327\r\nrst:0x15\r\n";
    for (size_t i = 0; boot[i]; i++)
        CHECK_EQ(esw_frame_rx_byte(&rx, (uint8_t)boot[i], out, sizeof(out)), 0);
    uint8_t p[5] = { 1, 0x02, 0, 0xAA, 0 };
    size_t fl = esw_frame_encode(p, 5, frame, sizeof(frame));
    frame[2] ^= 0x40;   /* corrupt */
    int got = 0;
    for (size_t i = 0; i < fl; i++) got = esw_frame_rx_byte(&rx, frame[i], out, sizeof(out));
    CHECK_EQ(got, 0);
    CHECK(rx.bad_frames >= 1);
    fl = esw_frame_encode(p, 5, frame, sizeof(frame));
    for (size_t i = 0; i < fl; i++) got = esw_frame_rx_byte(&rx, frame[i], out, sizeof(out));
    CHECK_EQ(got, 5);
    CHECK(memcmp(out, p, 5) == 0);
}

int main(void)
{
    RUN(crc_known_value);
    RUN(cobs_roundtrips);
    RUN(resync_after_garbage_and_bad_crc);
    return TT_RESULT();
}

/* SPDX-License-Identifier: MIT */
/* esp-swd host protocol: framing and command numbers (transport-agnostic).
 *
 * Wire frame:  COBS( payload || crc16_le(payload) ) 0x00
 * Request:     seq u8, cmd u8, args...
 * Response:    seq u8, cmd | 0x80, status i8 (esw_err_t), data...
 * All multi-byte fields little-endian. A receiver resynchronises at every 0x00, so stray bytes (boot ROM output)
 * cost one dropped frame, never a stuck link.
 */
#ifndef ESW_PROTO_H
#define ESW_PROTO_H
#include <stddef.h>
#include <stdint.h>

#define ESW_PROTO_VERSION      1u
#define ESW_PROTO_MAX_PAYLOAD  2304u                    /* seq + cmd + status + 2 KB data + headroom */
#define ESW_COBS_MAX(n)        ((n) + (n) / 254u + 2u)
#define ESW_FRAME_MAX          (ESW_COBS_MAX(ESW_PROTO_MAX_PAYLOAD + 2u) + 1u)

enum {
    ESW_CMD_PING = 0x01,        /* data echoed */
    ESW_CMD_INFO = 0x02,        /* -> proto u32, uptime_ms u32, reset_reason u32, free_heap u32, fw char[32] */
    ESW_CMD_REBOOT = 0x03,      /* mode u8: 0 normal, 1 ROM download */
    ESW_CMD_CH_HOLD = 0x10,     /* ch u8, group u8 */
    ESW_CMD_CH_RELEASE = 0x11,  /* ch u8, group u8 */
    ESW_CMD_CH_STATE = 0x12,    /* ch u8 -> vtref_mv i32, vtref_closed u8, swd u8, uart u8, pad u8,
                                         holders_swd u32, holders_uart u32 */
    ESW_CMD_SWD_ATTACH = 0x20,  /* ch u8, clock u32 [, phy u8: 0 bit-bang (clock = half-period loops), 1 dgpio
                                   (clock = Hz, 0 = 8 MHz)] -> dpidr u32, locked u8, nregions u8, khz u16,
                                   part char[32], nregions x {start u32, size u32, sector u32, kind u32} */
    ESW_CMD_SWD_DETACH = 0x21,  /* ch u8 */
    ESW_CMD_TGT_UNLOCK = 0x22,  /* ch u8, flags u32 */
    ESW_CMD_TGT_RESET = 0x23,   /* ch u8, flags u32 */
    ESW_CMD_MEM_READ = 0x24,    /* ch u8, addr u32, nwords u16 -> words */
    ESW_CMD_MEM_WRITE = 0x25,   /* ch u8, addr u32, words... */
    ESW_CMD_TGT_ERASE = 0x26,   /* ch u8, addr u32, len u32 */
    ESW_CMD_TGT_PROGRAM = 0x27, /* ch u8, addr u32, words... */
    ESW_CMD_TGT_VERIFY = 0x28,  /* ch u8, addr u32, words... */
    ESW_CMD_SWD_STATS = 0x29,   /* ch u8 -> xfers, wait, fault, noack, parity u32, last_op_us u32 */
    ESW_CMD_SWD_BENCH = 0x2A,   /* ch u8, mode u8 (0 DPIDR reads, 1 RAM block write, 2 RAM block read+check), n u32
                                   -> elapsed_us u32, errors u32, packets u32, wait u32, fault u32, noack u32,
                                      parity u32 (PHY benchmark on the device, no USB in the loop) */
    ESW_CMD_TGT_CRC32 = 0x2B,   /* ch u8, addr u32, len u32, flags u32 (bit0: reset-halt first) -> crc u32
                                   (zlib.crc32 of target memory; on-target loader where possible) */
};

uint16_t esw_crc16(const uint8_t *p, size_t n);   /* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) */

/* COBS: returns the encoded length (no delimiter). out must hold ESW_COBS_MAX(n). */
size_t esw_cobs_encode(const uint8_t *in, size_t n, uint8_t *out);
/* returns the decoded length, or -1 on malformed input / overflow */
int esw_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* payload -> complete frame including CRC and the trailing 0x00. Returns frame length, 0 if cap is too small. */
size_t esw_frame_encode(const uint8_t *payload, size_t n, uint8_t *out, size_t cap);

typedef struct {
    uint8_t raw[ESW_FRAME_MAX];
    size_t len;
    uint8_t overflow;
    uint32_t bad_frames;
} esw_frame_rx_t;

void esw_frame_rx_init(esw_frame_rx_t *rx);
/* Feed one byte. Returns the payload length (> 0) when a frame with a valid CRC completed, 0 otherwise. */
int esw_frame_rx_byte(esw_frame_rx_t *rx, uint8_t b, uint8_t *payload, size_t cap);

#endif

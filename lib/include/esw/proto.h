/* SPDX-License-Identifier: MIT */
/* esp-swd host protocol: framing and command numbers (transport-agnostic).
 *
 * Wire frame:  COBS( payload || crc16_le(payload) ) 0x00
 * Request:     seq u8, cmd u8, args...
 * Response:    seq u8, cmd | 0x80, status i8 (esw_err_t), data...
 * Event:       seq 0, cmd 0xE0..0xEF, status 0, data...   (device -> host, unsolicited; request commands stay below
 *              0x60 or equal 0x7E, so a response code never falls into the event range)
 * All multi-byte fields little-endian. A receiver resynchronises at every 0x00, so stray bytes (boot ROM output)
 * cost one dropped frame, never a stuck link.
 */
#ifndef ESW_PROTO_H
#define ESW_PROTO_H
#include <stddef.h>
#include <stdint.h>

#define ESW_PROTO_VERSION      5u   /* 2: UART bridge (0x30..0x33, event 0xE0); 3: raw DAP access (0x2C),
                                       attach core byte; 4: channels run in parallel, each with its own queue of
                                       ESW_PROTO_CH_QUEUE request frames (before: 3 frames in flight in total);
                                       5: switch modes (0x13), longer CH_STATE, UART internal loopback */
#define ESW_PROTO_CH_QUEUE     4u   /* proto >= 4: channel requests (0x10, 0x11, 0x13, 0x20..0x2F) the host may have in
                                       flight per channel; channel responses may overtake each other */
#define ESW_PROTO_MAX_PAYLOAD  2304u                    /* seq + cmd + status + 2 KB data + headroom */
#define ESW_COBS_MAX(n)        ((n) + (n) / 254u + 2u)
#define ESW_FRAME_MAX          (ESW_COBS_MAX(ESW_PROTO_MAX_PAYLOAD + 2u) + 1u)

enum {
    ESW_CMD_PING = 0x01,        /* data echoed */
    ESW_CMD_INFO = 0x02,        /* -> proto u32, uptime_ms u32, reset_reason u32, free_heap u32, fw char[32]
                                      [proto >= 5] boot_id u32 (random, new at every boot) */
    ESW_CMD_REBOOT = 0x03,      /* mode u8: 0 normal, 1 ROM download */
    ESW_CMD_CH_HOLD = 0x10,     /* ch u8, group u8 */
    ESW_CMD_CH_RELEASE = 0x11,  /* ch u8, group u8 */
    ESW_CMD_CH_STATE = 0x12,    /* ch u8 -> vtref_mv i32, vtref_closed u8, swd u8, uart u8 (esw_gstate_t),
                                         vtref_valid u8, holders_swd u32, holders_uart u32,
                                         [proto >= 5] mode_swd u8, mode_uart u8 (ESW_MODE_*), uart u8 (bit0 stream
                                         open, bit1 pads routed, bit2 loopback), attached u8 */
    ESW_CMD_CH_MODE = 0x13,     /* ch u8, group u8 (1 SWD, 2 UART), mode u8 (ESW_MODE_*): the host's standing
                                   order for a switch group. Interlock and VTref check apply in every mode. "off" on
                                   SWD ends an attached session first (debug off, pin reset); on UART it waits up to
                                   200 ms for TX to drain. "on" ends with host loss; "off" stays until changed or
                                   reboot. Runs on the channel's worker for SWD, on the dispatcher for UART (the
                                   host budgets it as a channel resp. a device command). */
    ESW_CMD_SWD_ATTACH = 0x20,  /* ch u8, clock u32 [, phy u8: 0 bit-bang (clock = half-period loops), 1 dgpio
                                   (clock = Hz, 0 = 8 MHz) [, core u8: multi-core parts, e.g. nRF5340 0 app,
                                   1 network; generic module: AP index; | 0x80: raw, generic module only]]
                                   -> dpidr u32, locked u8, nregions u8, khz u16,
                                   part char[32], nregions x {start u32, size u32, sector u32, kind u32} */
    ESW_CMD_SWD_DETACH = 0x21,  /* ch u8 */
    ESW_CMD_TGT_UNLOCK = 0x22,  /* ch u8, flags u32 -> as SWD_ATTACH: the memory map after the unlock */
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
    ESW_CMD_DAP_RAW = 0x2C,     /* ch u8, op u8 (0 DP read, 1 DP write, 2 AP read, 3 AP write), ap u8, reg u8,
                                   value u32 -> value u32. DP: reg bits 7:4 = DPBANKSEL (e.g. 0x24 = DPv2 TARGETID),
                                   SELECT is restored to bank 0 afterwards. Needs an attached session. */
    ESW_CMD_UART_OPEN = 0x30,   /* ch u8, baud u32, flags u32 (ESW_UART_*) -> actual baud u32, backend u8 (1 IRQ,
                                   2 DMA). (Re)configures the DUT UART, starts the RX stream and holds the UART group
                                   (ESW_HOLD_HOST_UART): the switches close once VTref is valid. Again on an open
                                   UART: new settings. The single DMA engine goes to the first channel opened while
                                   it is free; ESW_UART_NO_DMA keeps a channel on interrupts. */
    ESW_CMD_UART_CLOSE = 0x31,  /* ch u8: waits for TX to drain, stops the stream, releases the hold */
    ESW_CMD_UART_WRITE = 0x32,  /* ch u8, data... -> accepted u16 (less than sent when the TX buffer is full, or 0
                                   in the first ms after a closing: the host sends the rest later).
                                   ESW_ERR_INTERLOCK while the group is not closed. */
    ESW_CMD_UART_STATS = 0x33,  /* ch u8 -> open u8, routed u8, backend u8, pad u8, baud u32, rx u32, tx u32,
                                   usb_drop u32, fifo_ovf u32, buf_full u32, frame_err u32, parity_err u32, brk u32,
                                   dma_err u32 */
};

/* ESW_CMD_CH_MODE modes. Every boot starts in AUTO with nothing held: isolated. */
#define ESW_MODE_AUTO 0u   /* closed while something uses the group: an SWD session/job, a UART stream */
#define ESW_MODE_ON   1u   /* closed whenever the DUT is powered (VTref valid), used or not */
#define ESW_MODE_OFF  2u   /* never closed: attach and UART writes fail with ESW_ERR_INTERLOCK */

/* Events (device -> host). */
enum {
    ESW_EVT_UART_RX = 0xE0,     /* ch u8, flags u8 (ESW_UART_RX_*), ts_us u32 (device time when read), data... */
};

/* ESW_CMD_UART_OPEN flags: 8 data bits always. */
#define ESW_UART_PARITY_MASK  3u           /* 0 none, 1 even, 2 odd */
#define ESW_UART_STOP2        (1u << 2)    /* two stop bits */
#define ESW_UART_RX_NOFILT    (1u << 3)    /* RX glitch filter off (default on: pulses under 1/8 bit are ignored) */
#define ESW_UART_TX_STRONG    (1u << 4)    /* TX at full drive strength (default weakest: slower edges, less
                                              crosstalk into RX over plain jumper wires) */
#define ESW_UART_NO_DMA       (1u << 5)    /* keep this channel on the interrupt-driven back-end */
#define ESW_UART_LOOPBACK     (1u << 6)    /* self-test: TX looped to RX inside the UART; the pads stay on their
                                              safe idle state and the switch group is not held (no DUT needed) */
/* ESW_EVT_UART_RX flags: something happened since the previous RX event of this channel. */
#define ESW_UART_RX_LOST      (1u << 0)    /* bytes were lost: UART FIFO overflow, RX buffer full, or USB backlog */
#define ESW_UART_RX_ERROR     (1u << 1)    /* framing or parity error */
#define ESW_UART_RX_BREAK     (1u << 2)    /* break condition */

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

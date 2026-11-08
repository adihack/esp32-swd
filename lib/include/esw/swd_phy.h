/* SPDX-License-Identifier: MIT */
/* esp-swd: SWD PHY interface (packet level).
 *
 * The ADIv5 layer talks to the wire only through this interface, so a fast port-specific engine (dedicated GPIO,
 * SPI + DMA, another MCU's peripheral) can replace the portable bit-bang engine without touching anything above it.
 */
#ifndef ESW_SWD_PHY_H
#define ESW_SWD_PHY_H
#include <stdint.h>

/* Raw ACK values (3 bits, LSB first on the wire) and one pseudo value. */
#define ESW_ACK_OK      0x1u
#define ESW_ACK_WAIT    0x2u
#define ESW_ACK_FAULT   0x4u
#define ESW_ACK_PARITY  0x10u   /* ACK was OK but the read-data parity did not match */

/* Request byte bits. */
#define ESW_REQ_APNDP   0x02u
#define ESW_REQ_RNW     0x04u

static inline uint32_t esw_parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0xFu;
    return (0x6996u >> v) & 1u;
}

/* Request byte: start(1) APnDP RnW A2 A3 parity stop(0) park(1), sent LSB first.
 * addr is the register byte offset 0x0/0x4/0x8/0xC; only A[3:2] go on the wire. */
static inline uint8_t esw_swd_request(int apndp, int rnw, uint8_t addr)
{
    uint32_t ap = apndp ? 1u : 0u, rw = rnw ? 1u : 0u;
    uint32_t a2 = (addr >> 2) & 1u, a3 = (addr >> 3) & 1u;
    uint32_t par = (ap + rw + a2 + a3) & 1u;
    return (uint8_t)(0x81u | (ap << 1) | (rw << 2) | (a2 << 3) | (a3 << 4) | (par << 5));
}

typedef struct {
    /* One packet. Reads (req & ESW_REQ_RNW) store the value in *data; writes send *data.
     * Returns ESW_ACK_OK / WAIT / FAULT / PARITY, or the raw invalid ACK (0x7 = nobody drove SWDIO).
     * Must return in bounded time. */
    uint32_t (*transfer)(void *ctx, uint8_t req, uint32_t *data);
    /* Clock out nbits from bits[] (bit 0 of bits[0] first) with SWDIO driven: line reset, JTAG-to-SWD, idle. */
    void (*sequence)(void *ctx, const uint8_t *bits, uint32_t nbits);
    /* End of session: SWCLK low, SWDIO released (tool-side safe state). */
    void (*release)(void *ctx);
    /* Optional (NULL if fixed): request a clock in Hz, returns the clock actually used. */
    uint32_t (*set_clock)(void *ctx, uint32_t hz);
} esw_swd_phy_ops_t;

typedef struct {
    const esw_swd_phy_ops_t *ops;
    void *ctx;
} esw_swd_phy_t;

#endif

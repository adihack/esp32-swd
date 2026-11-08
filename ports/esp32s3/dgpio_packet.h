/* SPDX-License-Identifier: MIT */
/* dgpio_packet.h - SWD packet sequencer for a CPU-port GPIO engine (pure C, no IDF headers).
 *
 * The same code runs on the ESP32-S3 dedicated-GPIO port (esw_dgpio.c) and against the bit-level simulator in the
 * host tests (tests/test_dgpio.c). The includer defines the platform macros before including this file:
 *   DG_OUT(v, m)          write bits m of the CPU GPIO port to v (one atomic instruction on the S3)
 *   DG_IN()               read the CPU GPIO input port
 *   DG_CC()               free-running cycle counter (CCOUNT)
 *   DG_OE(e, on)          SWDIO output enable on/off, must not return before it is effective (fence)
 *   DG_IRQ_SAVE(e)        mask interrupts on this core if e->mask_irq, returns the previous state
 *   DG_IRQ_RESTORE(e, s)
 *   DG_INLINE             always-inline attribute
 *
 * Bit timing is CMSIS-DAP SW_DP order, identical to lib/core/swd/swd_bitbang.c: the host changes SWDIO while
 * SWCLK is low and the target samples on the rising edge; the host samples SWDIO at the end of the low phase. Edges
 * are paced by absolute cycle deadlines (t += half), re-based after every OE fence.
 */
#ifndef DGPIO_PACKET_H
#define DGPIO_PACKET_H
#include <stdint.h>
#include "esw/swd_phy.h"
#include "dgpio_types.h"



/* Wait for an absolute deadline. If the wait ends more than DG_LATE_REBASE cycles late (an interrupt, a stall), the
 * schedule restarts from now: a stall stretches only the current phase. Without this, every following phase whose
 * deadline already passed would come out back-to-back at the CPU's speed (12-16 MHz), above the target's limit. */
#ifndef DG_LATE_REBASE
#define DG_LATE_REBASE 16
#endif
#define DG_WAIT_UNTIL(t)                                                                            \
    do {                                                                                            \
        uint32_t dg_now_;                                                                           \
        while ((int32_t)((dg_now_ = DG_CC()) - (t)) < 0) {                                          \
        }                                                                                           \
        if ((int32_t)(dg_now_ - (t)) > DG_LATE_REBASE)                                              \
            (t) = dg_now_;                                                                          \
    } while (0)

static DG_INLINE uint32_t dg_wr(const dg_t *e, uint32_t t, uint32_t v, uint32_t n)
{
    const uint32_t h = e->half, c = e->clk_m, b = e->both_m, sh = e->dio_sh;
    while (n--) {
        DG_OUT((v & 1u) << sh, b);   /* SWCLK low and the data bit in one write */
        v >>= 1;
        t += h;
        DG_WAIT_UNTIL(t);
        DG_OUT(c, c);                /* SWCLK high: target samples */
        t += h;
        DG_WAIT_UNTIL(t);
    }
    return t;
}

static DG_INLINE uint32_t dg_rd(const dg_t *e, uint32_t *tp, uint32_t n)
{
    const uint32_t h = e->half, c = e->clk_m, s = e->in_sh;
    uint32_t t = *tp, v = 0;
    for (uint32_t i = 0; i < n; i++) {
        DG_OUT(0, c);                            /* SWCLK low */
        t += h;
        DG_WAIT_UNTIL(t);
        v |= ((DG_IN() >> s) & 1u) << i;         /* sample at the end of the low phase */
        DG_OUT(c, c);                            /* rising edge: target launches the next bit */
        t += h;
        DG_WAIT_UNTIL(t);
    }
    *tp = t;
    return v;
}

static DG_INLINE uint32_t dg_clocks(const dg_t *e, uint32_t t, uint32_t n)
{
    const uint32_t h = e->half, c = e->clk_m;
    while (n--) {
        DG_OUT(0, c);
        t += h;
        DG_WAIT_UNTIL(t);
        DG_OUT(c, c);
        t += h;
        DG_WAIT_UNTIL(t);
    }
    return t;
}

/* level first, then output enable: no glitch on SWDIO */
static DG_INLINE void dg_drive(dg_t *e, uint32_t level)
{
    DG_OUT(level ? e->dio_m : 0u, e->dio_m);
    if (!e->driving)
        DG_OE(e, 1);
}

static DG_INLINE void dg_release_dio(dg_t *e)
{
    if (e->driving)
        DG_OE(e, 0);
}

static DG_INLINE uint32_t dg_transfer(dg_t *e, uint8_t req, uint32_t *data)
{
    uint32_t irq = DG_IRQ_SAVE(e);
    e->oe_fault = 0;
    e->driving = 0;   /* re-assert output enable: pin setup may have changed it behind our back */
    dg_drive(e, 1);
    uint32_t t = dg_wr(e, DG_CC(), req, 8);
    dg_release_dio(e);
    t = dg_clocks(e, DG_CC(), e->turnaround);   /* re-base after the OE fence */
    uint32_t ack = dg_rd(e, &t, 3);

    if (ack == ESW_ACK_OK) {
        if (req & ESW_REQ_RNW) {
            uint32_t v = dg_rd(e, &t, 32);
            uint32_t p = dg_rd(e, &t, 1);
            t = dg_clocks(e, t, e->turnaround);
            dg_drive(e, 0);
            *data = v;
            if (p != esw_parity32(v))
                ack = ESW_ACK_PARITY;
        } else {
            uint32_t v = *data;
            t = dg_clocks(e, t, e->turnaround);
            dg_drive(e, v & 1u);
            t = dg_wr(e, DG_CC(), v, 32);
            t = dg_wr(e, t, esw_parity32(v), 1);
        }
        DG_OUT(0, e->dio_m);
        (void)dg_clocks(e, DG_CC(), e->idle_cycles);
    } else if (ack == ESW_ACK_WAIT || ack == ESW_ACK_FAULT) {
        if (e->data_phase && (req & ESW_REQ_RNW))
            t = dg_clocks(e, t, 33);
        t = dg_clocks(e, t, e->turnaround);
        dg_drive(e, 0);
        if (e->data_phase && !(req & ESW_REQ_RNW))
            (void)dg_wr(e, DG_CC(), 0, 33);
    } else {
        /* protocol error / no target: back off a whole data phase, then take the line back */
        (void)dg_clocks(e, t, e->turnaround + 33u);
        dg_drive(e, 1);
    }
    DG_IRQ_RESTORE(e, irq);
    if (e->oe_fault)
        ack = 0x7u;   /* the SWDIO output enable did not take: report a protocol error, never hang */
    return ack;
}

static DG_INLINE void dg_sequence(dg_t *e, const uint8_t *bits, uint32_t nbits)
{
    if (!nbits)
        return;
    uint32_t irq = DG_IRQ_SAVE(e);
    e->driving = 0;
    dg_drive(e, bits[0] & 1u);
    uint32_t t = DG_CC();
    for (uint32_t i = 0; i < nbits; i += 8) {
        uint32_t k = nbits - i < 8u ? nbits - i : 8u;
        t = dg_wr(e, t, bits[i >> 3], k);
    }
    DG_IRQ_RESTORE(e, irq);
}

static DG_INLINE void dg_release(dg_t *e)
{
    DG_OUT(0, e->clk_m);   /* SWCLK low */
    dg_release_dio(e);
}

#endif

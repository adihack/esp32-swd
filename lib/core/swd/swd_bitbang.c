/* SPDX-License-Identifier: MIT */
#include "esw/swd_bitbang.h"
#include <stddef.h>

#define P (bb->pins)

static inline void half(const esw_bitbang_t *bb)
{
    if (P->half_period)
        P->half_period(bb->ctx);
}

static void clock_cycles(esw_bitbang_t *bb, uint32_t n)
{
    while (n--) {
        P->swclk(bb->ctx, 0);
        half(bb);
        P->swclk(bb->ctx, 1);
        half(bb);
    }
}

/* Set the level first, then switch to output: no glitch on SWDIO. */
static void drive(esw_bitbang_t *bb, uint32_t level)
{
    P->swdio(bb->ctx, level);
    if (!bb->driving) {
        P->swdio_drive(bb->ctx, 1);
        bb->driving = 1;
    }
}

static void release_dio(esw_bitbang_t *bb)
{
    if (bb->driving) {
        P->swdio_drive(bb->ctx, 0);
        bb->driving = 0;
    }
}

static void write_bits(esw_bitbang_t *bb, uint32_t v, uint32_t n)
{
    while (n--) {
        P->swdio(bb->ctx, v & 1u);
        P->swclk(bb->ctx, 0);
        half(bb);
        P->swclk(bb->ctx, 1);
        half(bb);
        v >>= 1;
    }
}

static uint32_t read_bits(esw_bitbang_t *bb, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        P->swclk(bb->ctx, 0);
        half(bb);
        if (P->swdio_read(bb->ctx))
            v |= 1u << i;
        P->swclk(bb->ctx, 1);
        half(bb);
    }
    return v;
}

static uint32_t bb_transfer(void *ctx, uint8_t req, uint32_t *data)
{
    esw_bitbang_t *bb = ctx;
    bb->driving = 0;   /* re-assert output enable: someone else (pin setup, interlock) may have changed it */
    drive(bb, 1);
    write_bits(bb, req, 8);
    release_dio(bb);
    clock_cycles(bb, bb->turnaround);
    uint32_t ack = read_bits(bb, 3);

    if (ack == ESW_ACK_OK) {
        if (req & ESW_REQ_RNW) {
            uint32_t v = read_bits(bb, 32);
            uint32_t p = read_bits(bb, 1);
            clock_cycles(bb, bb->turnaround);
            drive(bb, 0);
            *data = v;
            if (p != esw_parity32(v))
                ack = ESW_ACK_PARITY;
        } else {
            clock_cycles(bb, bb->turnaround);
            drive(bb, 0);
            write_bits(bb, *data, 32);
            write_bits(bb, esw_parity32(*data), 1);
        }
        P->swdio(bb->ctx, 0);
        clock_cycles(bb, bb->idle_cycles);
        return ack;
    }
    if (ack == ESW_ACK_WAIT || ack == ESW_ACK_FAULT) {
        if (bb->data_phase && (req & ESW_REQ_RNW))
            clock_cycles(bb, 33);
        clock_cycles(bb, bb->turnaround);
        drive(bb, 0);
        if (bb->data_phase && !(req & ESW_REQ_RNW))
            write_bits(bb, 0, 33);
        return ack;
    }
    /* protocol error or no target: back off a whole data phase, then take the line back */
    clock_cycles(bb, bb->turnaround + 33u);
    drive(bb, 1);
    return ack;
}

static void bb_sequence(void *ctx, const uint8_t *bits, uint32_t nbits)
{
    esw_bitbang_t *bb = ctx;
    bb->driving = 0;
    for (uint32_t i = 0; i < nbits; i++) {
        drive(bb, ((uint32_t)bits[i >> 3] >> (i & 7u)) & 1u);
        P->swclk(bb->ctx, 0);
        half(bb);
        P->swclk(bb->ctx, 1);
        half(bb);
    }
}

static void bb_release(void *ctx)
{
    esw_bitbang_t *bb = ctx;
    P->swclk(bb->ctx, 0);
    release_dio(bb);
}

static const esw_swd_phy_ops_t bb_ops = {
    .transfer = bb_transfer,
    .sequence = bb_sequence,
    .release = bb_release,
    .set_clock = NULL,
};

void esw_bitbang_init(esw_bitbang_t *bb, const esw_bitbang_pins_t *pins, void *ctx)
{
    bb->pins = pins;
    bb->ctx = ctx;
    bb->turnaround = 1;
    bb->idle_cycles = 2;
    bb->data_phase = 0;
    bb->driving = 0;
}

esw_swd_phy_t esw_bitbang_phy(esw_bitbang_t *bb)
{
    esw_swd_phy_t phy = { &bb_ops, bb };
    return phy;
}

/* SPDX-License-Identifier: MIT */
/* esp-swd: portable SWD packet engine over four pin operations.
 *
 * Bit timing follows CMSIS-DAP SW_DP: the host changes SWDIO while SWCLK is low, the target samples on the rising
 * edge; the host samples SWDIO after the falling edge. Used by the simulator, as a fallback on any MCU, and as the
 * reference for fast engines. Proven on r5 hardware against an nRF52840 (MicroPython version, firmware/poc_swd).
 */
#ifndef ESW_SWD_BITBANG_H
#define ESW_SWD_BITBANG_H
#include <stdint.h>
#include "esw/swd_phy.h"

typedef struct {
    void (*swclk)(void *ctx, uint32_t level);
    void (*swdio)(void *ctx, uint32_t level);        /* output level (applies while driving) */
    uint32_t (*swdio_read)(void *ctx);
    void (*swdio_drive)(void *ctx, uint32_t drive);  /* 1: output, 0: input (released) */
    void (*half_period)(void *ctx);                  /* optional delay, NULL = as fast as possible */
} esw_bitbang_pins_t;

typedef struct {
    const esw_bitbang_pins_t *pins;
    void *ctx;
    uint8_t turnaround;   /* turnaround cycles (DP default 1) */
    uint8_t idle_cycles;  /* idle cycles after each OK packet */
    uint8_t data_phase;   /* 1: clock a data phase after WAIT/FAULT (required with overrun detection) */
    uint8_t driving;      /* internal: current SWDIO direction */
} esw_bitbang_t;

void esw_bitbang_init(esw_bitbang_t *bb, const esw_bitbang_pins_t *pins, void *ctx);
esw_swd_phy_t esw_bitbang_phy(esw_bitbang_t *bb);

#endif

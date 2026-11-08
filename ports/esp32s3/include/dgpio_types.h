/* SPDX-License-Identifier: MIT */
/* dgpio_types.h - state of the CPU-port SWD packet engine (see dgpio_packet.h). */
#ifndef DGPIO_TYPES_H
#define DGPIO_TYPES_H
#include <stdint.h>

typedef struct {
    uint32_t clk_m, dio_m, both_m;   /* bits of SWCLK / SWDIO in the CPU GPIO port */
    uint32_t dio_sh;                 /* bit index of SWDIO in the output port */
    uint32_t in_sh;                  /* bit index of SWDIO in the input port */
    uint32_t pad_m;                  /* SWDIO pad bit for the output-enable register (platform use) */
    uint32_t half;                   /* half SWCLK period in cycles */
    uint8_t turnaround, idle_cycles, data_phase, driving, mask_irq, oe_fault;
} dg_t;

#endif

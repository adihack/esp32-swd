/* SPDX-License-Identifier: MIT */
/* esp-swd board: dual SWD flasher r5 (design revision d). Source of truth: docs/hardware-reference.md. */
#ifndef BOARD_R5_H
#define BOARD_R5_H
#include <stdint.h>
#include "esw/channel.h"
#include "board_r5_pins.h"

#define BOARD_NUM_CHANNELS 2
#define BOARD_LED_GPIO R5_LED

typedef struct {
    char name;
    uint32_t swdio, swclk, swo, nrst_drv, nrst_sense, uart_tx, uart_rx, vtref_adc;
    uint32_t en_swd, en_uart, en_vtref;
} board_ch_t;

extern const board_ch_t board_ch[BOARD_NUM_CHANNELS];
extern const esw_channel_ops_t board_channel_ops;   /* ctx = const board_ch_t * */
extern const esw_channel_cfg_t board_channel_cfg;

/* First call in app_main: every isolation enable and reset driver an output driven low (switches open). */
void board_safe_init(void);
void board_adc_init(void);
void board_nrst_pulse(const board_ch_t *c, uint32_t ms);   /* only reaches the DUT while EN_SWD is closed */
void board_led(uint32_t on);
/* Register-only, IRAM, no locks: drives every enable and reset driver low (signal groups, resets, VTref last).
 * Safe from the panic handler and shutdown handlers. Pins stay outputs, actively low. */
void board_isolate_now(void);

#endif

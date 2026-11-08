/* SPDX-License-Identifier: MIT */
/* esp-swd port: ESP32-S3 dedicated-GPIO SWD engine ("dgpio").
 *
 * SWCLK and SWDIO are driven by the CPU's dedicated GPIO port (ee.wr_mask_gpio_out / ee.get_gpio_in: one cycle,
 * no APB access), edges are paced by CCOUNT, the packet code runs from IRAM with interrupts masked on this core.
 * The only bus accesses per packet are the two SWDIO output-enable writes at the turnarounds.
 *
 * Rules:
 *  - create and use an engine on ONE core (dedicated GPIO is per CPU): esw_dgpio_create() binds it to the calling
 *    core, every later call must run there (checked at attach).
 *  - board code reconfigures the pads (gpio_config) when an isolation group closes, which silently re-routes them
 *    away from the dedicated signals: call esw_dgpio_attach() after every group close, before using the PHY.
 *  - create the bundles at boot and never delete them (shared reset/clock of the dedicated GPIO block).
 */
#ifndef ESW_DGPIO_H
#define ESW_DGPIO_H
#include <stdint.h>
#include "esp_err.h"
#include "esw/err.h"
#include "esw/swd_phy.h"

#include "dgpio_types.h"

typedef struct {
    dg_t dg;                       /* hot state used by the packet loops */
    uint32_t clk_in_sh;            /* SWCLK bit in the input port (loopback self-check) */
    uint32_t core, swclk, swdio, sig_clk, sig_dio;
    uint32_t hz;                   /* actual SWCLK */
    void *bundle;                  /* dedic_gpio_bundle_handle_t */
} esw_dgpio_t;

/* Boot, on the owning core, with the isolation switches open. */
esp_err_t esw_dgpio_create(esw_dgpio_t *e, uint32_t swclk, uint32_t swdio);
/* Every attach: owner check, re-route both pads to the dedicated signals, self-check (routing read-back and a
 * SWCLK loopback), set the clock. Returns ESW_ERR_STATE if the engine cannot drive the pins. */
esw_err_t esw_dgpio_attach(esw_dgpio_t *e, uint32_t hz);
uint32_t esw_dgpio_set_clock(esw_dgpio_t *e, uint32_t hz);   /* returns the actual clock */
esw_swd_phy_t esw_dgpio_phy(esw_dgpio_t *e);

#endif

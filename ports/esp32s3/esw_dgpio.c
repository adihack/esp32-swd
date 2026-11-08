/* SPDX-License-Identifier: MIT */
/* ESP32-S3 dedicated-GPIO SWD engine. Design: docs/firmware-architecture.md, research notes 2026-11-06. */
#include <string.h>
#include "esw_dgpio.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "hal/dedic_gpio_periph.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_cpu.h"
#include "esp_attr.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"

/* ---- platform macros for the shared sequencer ---------------------------------------------------------- */
#define DG_INLINE inline __attribute__((always_inline))
#define DG_OUT(v, m) __asm__ volatile("ee.wr_mask_gpio_out %0, %1" ::"r"(v), "r"(m))
#define DG_IN() ({ uint32_t v_; __asm__ volatile("ee.get_gpio_in %0" : "=r"(v_)); v_; })
#define DG_CC() ({ uint32_t c_; __asm__ volatile("rsr.ccount %0" : "=r"(c_)); c_; })
/* SWDIO output enable over APB: W1TS/W1TC are atomic per bit, and the read-back is the fence: the core's write
 * buffer must not let the OE change land after the next clock edge. Bounded: if another writer (board pin setup on
 * the other core) undoes it, the packet ends as a protocol error instead of spinning with interrupts masked. */
#define DG_OE(e, on)                                                                                \
    do {                                                                                            \
        REG_WRITE((on) ? GPIO_ENABLE_W1TS_REG : GPIO_ENABLE_W1TC_REG, (e)->pad_m);                  \
        uint32_t dg_n_ = 64;                                                                        \
        while (((REG_READ(GPIO_ENABLE_REG) & (e)->pad_m) != 0u) != ((on) != 0) && --dg_n_) {       \
        }                                                                                           \
        if (!dg_n_)                                                                                 \
            (e)->oe_fault = 1;                                                                      \
        (e)->driving = (on) ? 1u : 0u;                                                              \
    } while (0)
#define DG_IRQ_SAVE(e) ((e)->mask_irq ? (uint32_t)portSET_INTERRUPT_MASK_FROM_ISR() : 0u)
#define DG_IRQ_RESTORE(e, s)                                                                        \
    do {                                                                                            \
        if ((e)->mask_irq)                                                                          \
            portCLEAR_INTERRUPT_MASK_FROM_ISR(s);                                                   \
    } while (0)
#include "dgpio_packet.h"

/* ---- PHY ops (IRAM: the packet must not stall on a flash cache miss) ----------------------------------- */
static IRAM_ATTR uint32_t op_transfer(void *ctx, uint8_t req, uint32_t *data)
{
    return dg_transfer(&((esw_dgpio_t *)ctx)->dg, req, data);
}

static IRAM_ATTR void op_sequence(void *ctx, const uint8_t *bits, uint32_t nbits)
{
    dg_sequence(&((esw_dgpio_t *)ctx)->dg, bits, nbits);
}

static IRAM_ATTR void op_release(void *ctx)
{
    dg_release(&((esw_dgpio_t *)ctx)->dg);
}

static uint32_t op_set_clock(void *ctx, uint32_t hz)
{
    return esw_dgpio_set_clock(ctx, hz);
}

static const esw_swd_phy_ops_t dgpio_ops = { op_transfer, op_sequence, op_release, op_set_clock };

esw_swd_phy_t esw_dgpio_phy(esw_dgpio_t *e)
{
    esw_swd_phy_t p = { &dgpio_ops, e };
    return p;
}

/* ---- clock --------------------------------------------------------------------------------------------- */
#define DGPIO_MAX_HZ 10000000u   /* board path limit (33R + TMUX1511 + ESD + cable), hardware-reference.md §7 */

uint32_t esw_dgpio_set_clock(esw_dgpio_t *e, uint32_t hz)
{
    const uint32_t f = (uint32_t)esp_clk_cpu_freq();
    if (hz == 0)
        hz = 8000000u;
    if (hz > DGPIO_MAX_HZ)
        hz = DGPIO_MAX_HZ;
    uint32_t h = (f + 2u * hz - 1u) / (2u * hz);   /* round up: never faster than requested */
    if (h < 12u)
        h = 12u;
    if (h > 0xFFFFu)
        h = 0xFFFFu;
    e->dg.half = h;
    /* mask interrupts only while a packet is short (<= ~100 us): about 48 clocks of 2h cycles */
    e->dg.mask_irq = (96u * h) <= 100u * (f / 1000000u);
    e->hz = f / (2u * h);
    return e->hz;
}

/* ---- create / attach ----------------------------------------------------------------------------------- */
esp_err_t esw_dgpio_create(esw_dgpio_t *e, uint32_t swclk, uint32_t swdio)
{
    memset(e, 0, sizeof(*e));
    if (swdio >= 32)
        return ESP_ERR_INVALID_ARG;   /* the OE fence uses GPIO_ENABLE_REG (pins 0-31); r5: GPIO4, GPIO11 */
    e->core = (uint32_t)esp_cpu_get_core_id();
    e->swclk = swclk;
    e->swdio = swdio;
    int pins[2] = { (int)swdio, (int)swclk };
    dedic_gpio_bundle_config_t bc = { .gpio_array = pins, .array_size = 2, .flags = { .in_en = 1, .out_en = 1 } };
    dedic_gpio_bundle_handle_t h = NULL;
    esp_err_t r = dedic_gpio_new_bundle(&bc, &h);
    if (r != ESP_OK)
        return r;
    e->bundle = h;
    uint32_t oo = 0, io = 0;
    dedic_gpio_get_out_offset(h, &oo);
    dedic_gpio_get_in_offset(h, &io);
    e->dg.dio_m = 1u << oo;
    e->dg.clk_m = 1u << (oo + 1u);
    e->dg.both_m = e->dg.dio_m | e->dg.clk_m;
    e->dg.dio_sh = oo;
    e->dg.in_sh = io;
    e->clk_in_sh = io + 1u;
    e->dg.pad_m = 1u << swdio;
    e->sig_dio = (uint32_t)dedic_gpio_periph_signals.cores[e->core].out_sig_per_channel[oo];
    e->sig_clk = (uint32_t)dedic_gpio_periph_signals.cores[e->core].out_sig_per_channel[oo + 1u];
    e->dg.turnaround = 1;
    e->dg.idle_cycles = 2;
    /* the bundle starts out driving both pads (ROM gpio_matrix_out enables output): SWCLK 0, SWDIO released */
    DG_OUT(e->dg.dio_m, e->dg.both_m);
    gpio_ll_output_disable(&GPIO, swdio);
    gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)swdio, false, false);   /* OE from GPIO_ENABLE, not the signal */
    e->dg.driving = 0;
    esw_dgpio_set_clock(e, 1000000u);
    return ESP_OK;
}

esw_err_t esw_dgpio_attach(esw_dgpio_t *e, uint32_t hz)
{
    if (!e->bundle || (uint32_t)esp_cpu_get_core_id() != e->core)
        return ESW_ERR_STATE;   /* wrong core: ee.* would drive the other core's port */
    DG_OUT(e->dg.dio_m, e->dg.both_m);   /* preset: SWCLK 0, SWDIO 1 */
    /* SWDIO: released, OE from GPIO_ENABLE; route both pads back to the dedicated signals (board pin setup via
     * gpio_config re-routes them to the simple GPIO output). func_sel changes are glitch-free: same level. */
    gpio_ll_output_disable(&GPIO, e->swdio);
    gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)e->swdio, false, false);
    gpio_ll_set_output_signal_matrix_source(&GPIO, e->swdio, e->sig_dio, false);
    gpio_ll_set_output_signal_matrix_source(&GPIO, e->swclk, e->sig_clk, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, (uint8_t)e->swclk, false, false);
    gpio_ll_output_enable(&GPIO, e->swclk);
    gpio_ll_input_enable(&GPIO, e->swdio);
    gpio_ll_input_enable(&GPIO, e->swclk);
    gpio_ll_set_drive_capability(&GPIO, e->swclk, GPIO_DRIVE_CAP_0);
    gpio_ll_set_drive_capability(&GPIO, e->swdio, GPIO_DRIVE_CAP_0);
    e->dg.driving = 0;
    /* self-check: routing read-back, then a SWCLK loopback through the pad (one edge with SWDIO released: the line
     * reset that follows makes it harmless) */
    if (GPIO.func_out_sel_cfg[e->swclk].func_sel != e->sig_clk || GPIO.func_out_sel_cfg[e->swdio].func_sel != e->sig_dio)
        return ESW_ERR_STATE;
    DG_OUT(e->dg.clk_m, e->dg.clk_m);
    for (volatile int i = 0; i < 4; i++) {
    }
    uint32_t hi = (DG_IN() >> e->clk_in_sh) & 1u;
    DG_OUT(0, e->dg.clk_m);
    for (volatile int i = 0; i < 4; i++) {
    }
    uint32_t lo = (DG_IN() >> e->clk_in_sh) & 1u;
    if (hi != 1u || lo != 0u)
        return ESW_ERR_STATE;
    esw_dgpio_set_clock(e, hz);
    return ESW_OK;
}

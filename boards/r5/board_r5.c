/* SPDX-License-Identifier: MIT */
#include "board_r5.h"
#include "esw_port.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

const board_ch_t board_ch[BOARD_NUM_CHANNELS] = {
    { 'A', .swdio = R5_A_SWDIO, .swclk = R5_A_SWCLK, .swo = R5_A_SWO, .nrst_drv = R5_A_NRST_DRV,
      .nrst_sense = R5_A_NRST_SENSE, .uart_tx = R5_A_UART_TX, .uart_rx = R5_A_UART_RX, .vtref_adc = R5_A_VTREF_ADC,
      .en_swd = R5_A_EN_SWD, .en_uart = R5_A_EN_UART, .en_vtref = R5_A_EN_VTREF },
    { 'B', .swdio = R5_B_SWDIO, .swclk = R5_B_SWCLK, .swo = R5_B_SWO, .nrst_drv = R5_B_NRST_DRV,
      .nrst_sense = R5_B_NRST_SENSE, .uart_tx = R5_B_UART_TX, .uart_rx = R5_B_UART_RX, .vtref_adc = R5_B_VTREF_ADC,
      .en_swd = R5_B_EN_SWD, .en_uart = R5_B_EN_UART, .en_vtref = R5_B_EN_VTREF },
};

/* VTref window for 3.3 V I/O (the nRF52840 DK sits at ~2.98 V), divider settle 10 tau. */
const esw_channel_cfg_t board_channel_cfg = { .vtref_min_mv = 2800, .vtref_max_mv = 3600, .vtref_settle_ms = 50,
                                              .host_loss_grace_ms = 2000 };

static void out_low(uint32_t pin)
{
    gpio_set_level(pin, 0);   /* level first, then direction: no high glitch */
    gpio_config_t c = { .pin_bit_mask = 1ull << pin, .mode = GPIO_MODE_INPUT_OUTPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}

static void input(uint32_t pin)
{
    gpio_config_t c = { .pin_bit_mask = 1ull << pin, .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}

void board_safe_init(void)
{
    for (int i = 0; i < BOARD_NUM_CHANNELS; i++) {
        const board_ch_t *c = &board_ch[i];
        out_low(c->en_swd);
        out_low(c->en_uart);
        out_low(c->en_vtref);
        out_low(c->nrst_drv);
        out_low(c->swclk);
        input(c->swdio);
        input(c->swo);
        input(c->nrst_sense);
        input(c->uart_rx);
        input(c->uart_tx);
    }
    out_low(BOARD_LED_GPIO);
}

void board_adc_init(void)
{
    esw_port_adc_init();
    for (int i = 0; i < BOARD_NUM_CHANNELS; i++)
        esw_port_adc_add(board_ch[i].vtref_adc);
}

void board_nrst_pulse(const board_ch_t *c, uint32_t ms)
{
    gpio_set_level(c->nrst_drv, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(c->nrst_drv, 0);
}

void IRAM_ATTR board_isolate_now(void)
{
    REG_WRITE(GPIO_OUT_W1TC_REG, R5_SIG_LO);
    REG_WRITE(GPIO_OUT1_W1TC_REG, R5_SIG_HI);
    REG_WRITE(GPIO_OUT_W1TC_REG, R5_NRST_LO);
    REG_WRITE(GPIO_OUT1_W1TC_REG, R5_NRST_HI);
    REG_WRITE(GPIO_OUT_W1TC_REG, R5_VTREF_LO);
    REG_WRITE(GPIO_OUT1_W1TC_REG, R5_VTREF_HI);
}

void board_led(uint32_t on)
{
    gpio_set_level(BOARD_LED_GPIO, on ? 1 : 0);
}

/* ---- channel ops ------------------------------------------------------------------------------------------- */

static void ch_set_switch(void *ctx, esw_group_t g, uint32_t closed)
{
    const board_ch_t *c = ctx;
    uint32_t pin = g == ESW_GROUP_VTREF ? c->en_vtref : g == ESW_GROUP_SWD ? c->en_swd : c->en_uart;
    gpio_set_level(pin, closed ? 1 : 0);
}

static int32_t ch_vtref(void *ctx)
{
    const board_ch_t *c = ctx;
    int32_t mv = esw_port_adc_read_mv(c->vtref_adc, 8);
    return mv < 0 ? -1 : 2 * mv;   /* 100k/100k divider */
}

static void ch_prepare(void *ctx, esw_group_t g)
{
    const board_ch_t *c = ctx;
    if (g == ESW_GROUP_SWD) {
        gpio_set_level(c->nrst_drv, 0);
        out_low(c->swclk);
        input(c->swdio);
    } else if (g == ESW_GROUP_UART) {
        gpio_set_level(c->uart_tx, 1);   /* UART idle high */
        gpio_config_t o = { .pin_bit_mask = 1ull << c->uart_tx, .mode = GPIO_MODE_INPUT_OUTPUT,
                            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .intr_type = GPIO_INTR_DISABLE };
        gpio_config(&o);
        input(c->uart_rx);
    }
}

const esw_channel_ops_t board_channel_ops = { ch_set_switch, ch_vtref, ch_prepare, NULL };

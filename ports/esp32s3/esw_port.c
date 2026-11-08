/* SPDX-License-Identifier: MIT */
#include "esw_port.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- SWD pins: direct GPIO register access (W1TS/W1TC), no driver calls in the bit loop ------------------- */

static inline void IRAM_ATTR out_set(uint32_t pin, uint32_t level)
{
    if (pin < 32)
        REG_WRITE(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1u << pin);
    else
        REG_WRITE(level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, 1u << (pin - 32));
}

static inline uint32_t IRAM_ATTR in_get(uint32_t pin)
{
    return pin < 32 ? (REG_READ(GPIO_IN_REG) >> pin) & 1u : (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1u;
}

static inline void IRAM_ATTR oe_set(uint32_t pin, uint32_t en)
{
    if (pin < 32)
        REG_WRITE(en ? GPIO_ENABLE_W1TS_REG : GPIO_ENABLE_W1TC_REG, 1u << pin);
    else
        REG_WRITE(en ? GPIO_ENABLE1_W1TS_REG : GPIO_ENABLE1_W1TC_REG, 1u << (pin - 32));
}

static void IRAM_ATTR p_swclk(void *ctx, uint32_t level) { out_set(((esw_port_swd_t *)ctx)->swclk, level); }
static void IRAM_ATTR p_swdio(void *ctx, uint32_t level) { out_set(((esw_port_swd_t *)ctx)->swdio, level); }
static uint32_t IRAM_ATTR p_read(void *ctx) { return in_get(((esw_port_swd_t *)ctx)->swdio); }
static void IRAM_ATTR p_drive(void *ctx, uint32_t drive) { oe_set(((esw_port_swd_t *)ctx)->swdio, drive); }

static void IRAM_ATTR p_half(void *ctx)
{
    for (volatile uint32_t i = ((esw_port_swd_t *)ctx)->half_loops; i; i--) {
    }
}

const esw_bitbang_pins_t esw_port_swd_pins = { p_swclk, p_swdio, p_read, p_drive, p_half };

void esw_port_swd_init(esw_port_swd_t *p, uint32_t swclk, uint32_t swdio, uint32_t half_loops)
{
    p->swclk = swclk;
    p->swdio = swdio;
    p->half_loops = half_loops;
    gpio_set_level(swclk, 0);
    gpio_set_level(swdio, 1);
    gpio_config_t c = {
        .pin_bit_mask = (1ull << swclk) | (1ull << swdio),
        .mode = GPIO_MODE_INPUT_OUTPUT,   /* input buffer always on: SWDIO is read back while released */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    gpio_set_drive_capability(swclk, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(swdio, GPIO_DRIVE_CAP_0);
    oe_set(swdio, 0);   /* SWDIO released until the engine drives it */
}

/* ---- VTref ADC ----------------------------------------------------------------------------------------- */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

esp_err_t esw_port_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    esp_err_t e = adc_oneshot_new_unit(&u, &s_adc);
    if (e != ESP_OK)
        return e;
    adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_DEFAULT };
    return adc_cali_create_scheme_curve_fitting(&cc, &s_cali);
}

esp_err_t esw_port_adc_add(uint32_t gpio)
{
    adc_unit_t unit;
    adc_channel_t ch;
    esp_err_t e = adc_oneshot_io_to_channel((int)gpio, &unit, &ch);
    if (e != ESP_OK || unit != ADC_UNIT_1)
        return e != ESP_OK ? e : ESP_ERR_INVALID_ARG;
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    return adc_oneshot_config_channel(s_adc, ch, &c);
}

int32_t esw_port_adc_read_mv(uint32_t gpio, uint32_t samples)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel((int)gpio, &unit, &ch) != ESP_OK)
        return -1;
    int32_t sum = 0;
    for (uint32_t i = 0; i < samples; i++) {
        int mv = 0;
        if (adc_oneshot_get_calibrated_result(s_adc, s_cali, ch, &mv) != ESP_OK)
            return -1;
        sum += mv;
    }
    return samples ? sum / (int32_t)samples : -1;
}

/* ---- clock ----------------------------------------------------------------------------------------------- */

static uint32_t c_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void c_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (ms == 0)
        taskYIELD();
    else
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

const esw_clock_t esw_port_clock = { c_now, c_sleep, NULL };

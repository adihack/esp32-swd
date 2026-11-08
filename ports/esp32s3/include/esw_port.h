/* SPDX-License-Identifier: MIT */
/* esp-swd port: ESP32-S3 (ESP-IDF). Fast GPIO bit-bang pins for the portable SWD engine, VTref ADC, clock. */
#ifndef ESW_PORT_H
#define ESW_PORT_H
#include <stdint.h>
#include "esw/swd_bitbang.h"
#include "esw/clock.h"
#include "esp_err.h"

typedef struct {
    uint32_t swclk, swdio;
    uint32_t half_loops;   /* busy-loop iterations per half clock period; 0 = as fast as the GPIO allows */
} esw_port_swd_t;

extern const esw_bitbang_pins_t esw_port_swd_pins;
/* Configure SWCLK (output, low) and SWDIO (input) with the weakest drive strength. */
void esw_port_swd_init(esw_port_swd_t *p, uint32_t swclk, uint32_t swdio, uint32_t half_loops);

/* VTref ADC (ADC1, 12 dB attenuation, curve-fitting calibration). */
esp_err_t esw_port_adc_init(void);
esp_err_t esw_port_adc_add(uint32_t gpio);                 /* register a GPIO as ADC1 input */
int32_t esw_port_adc_read_mv(uint32_t gpio, uint32_t samples);   /* calibrated pin voltage, mV; < 0 on error */

extern const esw_clock_t esw_port_clock;

#endif

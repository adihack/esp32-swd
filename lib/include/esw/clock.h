/* SPDX-License-Identifier: MIT */
/* esp-swd: time source supplied by the port (RTOS tick, esp_timer, a fake clock in tests). */
#ifndef ESW_CLOCK_H
#define ESW_CLOCK_H
#include <stdint.h>

typedef struct {
    uint32_t (*now_ms)(void *ctx);              /* monotonic milliseconds, wraps at 2^32 */
    void (*sleep_ms)(void *ctx, uint32_t ms);   /* may yield to other tasks */
    void *ctx;
} esw_clock_t;

static inline uint32_t esw_now_ms(const esw_clock_t *c) { return c->now_ms(c->ctx); }
static inline void esw_sleep_ms(const esw_clock_t *c, uint32_t ms) { c->sleep_ms(c->ctx, ms); }
static inline uint32_t esw_elapsed_ms(const esw_clock_t *c, uint32_t since) { return esw_now_ms(c) - since; }

#endif

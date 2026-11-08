/* SPDX-License-Identifier: MIT */
/* Shared test rig: simulator + bit-bang engine + DAP + fake clock. */
#ifndef RIG_H
#define RIG_H
#include <string.h>
#include "swd_sim.h"
#include "esw/swd_bitbang.h"
#include "esw/adiv5.h"
#include "esw/clock.h"

typedef struct {
    swd_sim_t sim;
    esw_bitbang_t bb;
    esw_dap_t dap;
    uint32_t t_ms;
    esw_clock_t clk;
} rig_t;

static uint32_t rig_now(void *ctx) { return ((rig_t *)ctx)->t_ms++; }   /* advances on every read */
static void rig_sleep(void *ctx, uint32_t ms) { ((rig_t *)ctx)->t_ms += ms; }

static rig_t g_rig;   /* the simulator is ~260 KB: keep it off the stack */

static rig_t *rig_init(uint32_t locked)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof(*r));
    swd_sim_init(&r->sim, locked);
    esw_bitbang_init(&r->bb, &swd_sim_pins, &r->sim);
    esw_dap_init(&r->dap, esw_bitbang_phy(&r->bb));
    r->clk.now_ms = rig_now;
    r->clk.sleep_ms = rig_sleep;
    r->clk.ctx = r;
    return r;
}

#endif

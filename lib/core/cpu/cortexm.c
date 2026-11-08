/* SPDX-License-Identifier: MIT */
#include "esw/cortexm.h"
#include <stddef.h>

/* generic register id -> DCRSR REGSEL */
static const uint8_t regsel[ESW_REG_COUNT] = {
    [ESW_REG_ARG0] = 0, [ESW_REG_ARG1] = 1, [ESW_REG_ARG2] = 2, [ESW_REG_ARG3] = 3,
    [ESW_REG_SB] = 9, [ESW_REG_SP] = 13, [ESW_REG_RA] = 14, [ESW_REG_PC] = 15, [ESW_REG_STATUS] = 16,
};

static esw_err_t dhcsr_write(esw_cpu_t *c, uint32_t bits)
{
    return esw_mem_write32(c->mem, ESW_CM_DHCSR, ESW_DHCSR_DBGKEY | bits);
}

static esw_err_t cm_halt(esw_cpu_t *c)
{
    ESW_TRY(dhcsr_write(c, ESW_DHCSR_C_DEBUGEN | ESW_DHCSR_C_HALT));
    return esw_mem_poll32(c->mem, ESW_CM_DHCSR, ESW_DHCSR_S_HALT, ESW_DHCSR_S_HALT, c->max_polls, NULL);
}

static esw_err_t cm_resume(esw_cpu_t *c)
{
    ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DFSR, 0x1Fu));   /* clear halt reasons (write-one-to-clear) */
    return dhcsr_write(c, ESW_DHCSR_C_DEBUGEN);
}

static esw_err_t cm_is_halted(esw_cpu_t *c, uint32_t *halted)
{
    uint32_t v = 0;
    ESW_TRY(esw_mem_read32(c->mem, ESW_CM_DHCSR, &v));
    *halted = (v & ESW_DHCSR_S_HALT) ? 1u : 0u;
    return ESW_OK;
}

esw_err_t esw_cortexm_reg_read(esw_cpu_t *c, uint32_t sel, uint32_t *val)
{
    ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DCRSR, sel & 0x7Fu));
    ESW_TRY(esw_mem_poll32(c->mem, ESW_CM_DHCSR, ESW_DHCSR_S_REGRDY, ESW_DHCSR_S_REGRDY, c->max_polls, NULL));
    return esw_mem_read32(c->mem, ESW_CM_DCRDR, val);
}

esw_err_t esw_cortexm_reg_write(esw_cpu_t *c, uint32_t sel, uint32_t val)
{
    ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DCRDR, val));
    ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DCRSR, (sel & 0x7Fu) | ESW_DCRSR_REGWNR));
    return esw_mem_poll32(c->mem, ESW_CM_DHCSR, ESW_DHCSR_S_REGRDY, ESW_DHCSR_S_REGRDY, c->max_polls, NULL);
}

static esw_err_t cm_reg_read(esw_cpu_t *c, esw_reg_t reg, uint32_t *val)
{
    if ((unsigned)reg >= ESW_REG_COUNT)
        return ESW_ERR_ARG;
    return esw_cortexm_reg_read(c, regsel[reg], val);
}

static esw_err_t cm_reg_write(esw_cpu_t *c, esw_reg_t reg, uint32_t val)
{
    if ((unsigned)reg >= ESW_REG_COUNT)
        return ESW_ERR_ARG;
    return esw_cortexm_reg_write(c, regsel[reg], val);
}

static esw_err_t cm_prepare_call(esw_cpu_t *c, uint32_t fn, const uint32_t args[4], uint32_t sp, uint32_t sb,
                                 uint32_t trap)
{
    for (uint32_t i = 0; i < 4; i++)
        ESW_TRY(esw_cortexm_reg_write(c, i, args ? args[i] : 0u));
    ESW_TRY(esw_cortexm_reg_write(c, 9, sb));
    ESW_TRY(esw_cortexm_reg_write(c, 13, sp));
    ESW_TRY(esw_cortexm_reg_write(c, 14, trap | 1u));        /* return in Thumb state into the BKPT */
    ESW_TRY(esw_cortexm_reg_write(c, 15, fn & ~1u));
    return esw_cortexm_reg_write(c, 16, ESW_XPSR_T);
}

#define ESW_DFSR_HALTED (1u << 0)
#define ESW_DFSR_VCATCH (1u << 3)

static esw_err_t cm_sys_reset(esw_cpu_t *c, uint32_t halt_after)
{
    uint32_t demcr = 0, dhcsr = 0;
    esw_err_t e = ESW_OK;
    if (halt_after) {
        /* Halt first: arming vector catch needs C_DEBUGEN, and writing C_DEBUGEN alone would RESUME a core that is
         * already halted (it would run erased or half-written code until the reset hits). */
        ESW_TRY(dhcsr_write(c, ESW_DHCSR_C_DEBUGEN | ESW_DHCSR_C_HALT));
        ESW_TRY(esw_mem_read32(c->mem, ESW_CM_DEMCR, &demcr));
        ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DEMCR, demcr | ESW_DEMCR_VC_CORERESET));
        ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DFSR, 0x1Fu));   /* clear halt reasons: VCATCH marks our reset halt */
    }
    (void)esw_mem_read32(c->mem, ESW_CM_DHCSR, &dhcsr);   /* reading clears the sticky S_RESET_ST */
    /* The write may not complete cleanly while the system resets: ignore its status, then resync. */
    (void)esw_mem_write32(c->mem, ESW_CM_AIRCR, ESW_AIRCR_SYSRESETREQ);
    esw_memap_invalidate(c->mem);
    (void)esw_dap_clear_errors(c->mem->dap);
    if (halt_after) {
        uint32_t seen_reset = 0, dfsr = 0;
        e = ESW_ERR_TIMEOUT;
        for (uint32_t i = 0; i < c->max_polls; i++) {
            if (esw_mem_read32(c->mem, ESW_CM_DHCSR, &dhcsr) != ESW_OK)
                continue;   /* bus busy or locked right after reset */
            seen_reset |= dhcsr & ESW_DHCSR_S_RESET_ST;
            if (!(dhcsr & ESW_DHCSR_S_HALT))
                continue;
            /* halted by the reset, not still halted from before: S_RESET_ST seen, or (if a lost read consumed that
             * clear-on-read bit) the vector catch recorded in DFSR */
            if (seen_reset || (esw_mem_read32(c->mem, ESW_CM_DFSR, &dfsr) == ESW_OK && (dfsr & ESW_DFSR_VCATCH))) {
                e = ESW_OK;
                break;
            }
        }
        /* restore DEMCR on every path (best effort) so a later normal reset does not halt */
        esw_err_t e2 = esw_mem_write32(c->mem, ESW_CM_DEMCR, demcr & ~ESW_DEMCR_VC_CORERESET);
        if (e == ESW_OK)
            e = e2;
    }
    return e;
}

static esw_err_t cm_call_guard(esw_cpu_t *c, uint32_t on)
{
    if (on && !c->guard_on) {
        ESW_TRY(esw_mem_read32(c->mem, ESW_CM_DEMCR, &c->saved_guard));
        ESW_TRY(esw_mem_write32(c->mem, ESW_CM_DEMCR,
                                c->saved_guard | ESW_DEMCR_VC_CORERESET | ESW_DEMCR_VC_HARDERR));
        c->guard_on = 1;
    } else if (!on && c->guard_on) {
        c->guard_on = 0;
        return esw_mem_write32(c->mem, ESW_CM_DEMCR, c->saved_guard);
    }
    return ESW_OK;
}

static esw_err_t cm_debug_off(esw_cpu_t *c)
{
    return dhcsr_write(c, 0);
}

static const esw_cpu_ops_t cortexm_ops = {
    .halt = cm_halt,
    .resume = cm_resume,
    .is_halted = cm_is_halted,
    .reg_read = cm_reg_read,
    .reg_write = cm_reg_write,
    .prepare_call = cm_prepare_call,
    .sys_reset = cm_sys_reset,
    .debug_off = cm_debug_off,
    .call_guard = cm_call_guard,
};

void esw_cortexm_init(esw_cpu_t *c, esw_memap_t *mem)
{
    c->ops = &cortexm_ops;
    c->mem = mem;
    c->max_polls = 1000;
    c->guard_on = 0;
}

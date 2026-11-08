/* SPDX-License-Identifier: MIT */
/* esp-swd: architecture-neutral core control. The loader runtime and target modules use only this interface;
 * Cortex-M is the first implementation (esw/cortexm.h). */
#ifndef ESW_CPU_H
#define ESW_CPU_H
#include <stdint.h>
#include "esw/err.h"
#include "esw/adiv5.h"

/* Generic register ids, mapped by each architecture. */
typedef enum {
    ESW_REG_ARG0 = 0, ESW_REG_ARG1, ESW_REG_ARG2, ESW_REG_ARG3,   /* call arguments; ARG0 also holds the result */
    ESW_REG_SB,          /* static base for position-independent loaders (Cortex-M: R9) */
    ESW_REG_SP,
    ESW_REG_RA,          /* return address (Cortex-M: LR) */
    ESW_REG_PC,
    ESW_REG_STATUS,      /* Cortex-M: xPSR */
    ESW_REG_COUNT
} esw_reg_t;

typedef struct esw_cpu esw_cpu_t;

typedef struct {
    esw_err_t (*halt)(esw_cpu_t *c);
    esw_err_t (*resume)(esw_cpu_t *c);
    esw_err_t (*is_halted)(esw_cpu_t *c, uint32_t *halted);
    esw_err_t (*reg_read)(esw_cpu_t *c, esw_reg_t reg, uint32_t *val);
    esw_err_t (*reg_write)(esw_cpu_t *c, esw_reg_t reg, uint32_t val);
    /* Set up registers so that resume() executes fn(args) and stops at trap (a breakpoint in target RAM). */
    esw_err_t (*prepare_call)(esw_cpu_t *c, uint32_t fn, const uint32_t args[4], uint32_t sp, uint32_t sb,
                              uint32_t trap);
    esw_err_t (*sys_reset)(esw_cpu_t *c, uint32_t halt_after);
    esw_err_t (*debug_off)(esw_cpu_t *c);   /* resume and leave halting debug */
    /* Optional (NULL if unsupported): while on, a reset or a fatal fault halts the core at once (Cortex-M: DEMCR
     * vector catch) instead of letting target firmware run; off restores the previous setting. */
    esw_err_t (*call_guard)(esw_cpu_t *c, uint32_t on);
} esw_cpu_ops_t;

struct esw_cpu {
    const esw_cpu_ops_t *ops;
    esw_memap_t *mem;
    uint32_t max_polls;        /* bound for register/halt handshakes (default 1000) */
    uint32_t saved_guard;      /* call_guard: previous setting (Cortex-M: DEMCR) */
    uint8_t guard_on;
};

#endif

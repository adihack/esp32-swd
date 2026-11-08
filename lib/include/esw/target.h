/* SPDX-License-Identifier: MIT */
/* esp-swd: target-module interface and registry.
 *
 * Chip or family support is a self-contained module implementing esw_target_driver_t (lib/targets/<name>/).
 * The common stack (SWD, ADIv5, core control, loader runtime) is shared. Each memory region is programmed either
 * directly by the module or through a loader descriptor handed to the common loader runtime.
 */
#ifndef ESW_TARGET_H
#define ESW_TARGET_H
#include <stdint.h>
#include "esw/err.h"
#include "esw/clock.h"
#include "esw/adiv5.h"
#include "esw/cpu.h"
#include "esw/loader.h"

#define ESW_TARGET_MAX_REGIONS 8

typedef enum { ESW_MEM_FLASH, ESW_MEM_RAM, ESW_MEM_OTP, ESW_MEM_EXT } esw_mem_kind_t;

typedef struct {
    const char *name;
    esw_mem_kind_t kind;
    uint32_t start, size;
    uint32_t sector_size;                 /* erase granularity, bytes */
    const esw_loader_desc_t *loader;      /* NULL: programmed directly by the module */
    uint32_t use_stream;                  /* loader regions: 1 = streaming ABI, 0 = call ABI */
} esw_mem_region_t;

typedef struct esw_target esw_target_t;

#define ESW_UNLOCK_ALLOW_ERASE  (1u << 0)   /* permit a destructive mass erase to regain access */
#define ESW_RESET_HALT          (1u << 0)   /* halt at the reset vector */

typedef struct {
    const char *name;
    /* After connect + power-up: ESW_OK if this module handles the target (DPIDR, AP IDRs, ID registers). */
    esw_err_t (*probe)(esw_target_t *t);
    /* Identify the part, fill t->part and t->regions, set t->locked. */
    esw_err_t (*attach)(esw_target_t *t);
    esw_err_t (*unlock)(esw_target_t *t, uint32_t flags);
    /* Direct regions only (loader regions are handled by the common runtime). */
    esw_err_t (*erase)(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, uint32_t len);
    esw_err_t (*program)(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, const uint32_t *data,
                         uint32_t nwords);
    esw_err_t (*reset)(esw_target_t *t, uint32_t flags);
    esw_err_t (*detach)(esw_target_t *t);   /* leave debug state cleanly (optional) */
} esw_target_driver_t;

struct esw_target {
    const esw_target_driver_t *drv;
    esw_dap_t *dap;
    const esw_clock_t *clk;
    esw_memap_t mem;
    esw_cpu_t cpu;
    esw_loader_t loader;
    const esw_loader_desc_t *loader_active;
    char part[32];
    uint32_t locked;
    uint32_t scratch_ram;                 /* ESW_CRC32_LDR_RAM bytes of RAM the probe may use for helpers, 0 = none */
    esw_mem_region_t regions[ESW_TARGET_MAX_REGIONS];
    uint32_t nregions;
    uint32_t priv[16];                    /* module-private scratch */
};

/* Registry: NULL-terminated table built from the enabled modules (lib/targets/registry.c). */
extern const esw_target_driver_t *const esw_target_drivers[];

/* Connect is done by the caller (esw_dap_connect + esw_dap_power_up). Tries every module's probe. */
esw_err_t esw_target_detect(esw_target_t *t, esw_dap_t *dap, const esw_clock_t *clk);
const esw_mem_region_t *esw_target_region(const esw_target_t *t, uint32_t addr, uint32_t len);
esw_err_t esw_target_add_region(esw_target_t *t, const esw_mem_region_t *r);
/* Generic operations: route to the module (direct) or to the loader runtime (loader regions). */
esw_err_t esw_target_erase(esw_target_t *t, uint32_t addr, uint32_t len);
esw_err_t esw_target_program(esw_target_t *t, uint32_t addr, const uint32_t *data, uint32_t nwords);
esw_err_t esw_target_verify(esw_target_t *t, uint32_t addr, const uint32_t *data, uint32_t nwords);
esw_err_t esw_target_finish(esw_target_t *t);   /* stop an active loader */

#define ESW_CRC_RESET_HALT (1u << 0)   /* reset-halt first: no firmware, DMA or interrupt touches the scratch RAM */
/* zlib.crc32 of target memory [addr, addr+len). Order of preference: a streaming loader's CRC command (loader
 * regions), the built-in on-target CRC loader (module scratch RAM, memory-mapped regions), read-back + CRC on the
 * probe (always works; also the fallback if the loader fails). */
esw_err_t esw_target_crc32(esw_target_t *t, uint32_t addr, uint32_t len, uint32_t flags, uint32_t *crc);

#endif

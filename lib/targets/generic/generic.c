/* SPDX-License-Identifier: MIT */
#include "esw/targets/generic.h"
#include "esw/cortexm.h"
#include <stdio.h>

#define AP_IDR_CLASS(idr) (((idr) >> 13) & 0xFu)
#define AP_CLASS_MEM      0x8u

static const char *core_name(uint32_t cpuid)
{
    switch ((cpuid >> 4) & 0xFFFu) {
    case 0xC20: return "Cortex-M0";
    case 0xC60: return "Cortex-M0+";
    case 0xC21: return "Cortex-M1";
    case 0xC23: return "Cortex-M3";
    case 0xC24: return "Cortex-M4";
    case 0xC27: return "Cortex-M7";
    case 0xD20: return "Cortex-M23";
    case 0xD21: return "Cortex-M33";
    case 0xD22: return "Cortex-M55";
    case 0xD23: return "Cortex-M85";
    case 0xD24: return "Cortex-M52";
    default: return "Cortex-M?";
    }
}

/* Any target whose AP reads back: the registry tries this module last. Core n = AP n. */
static esw_err_t generic_probe(esw_target_t *t)
{
    uint32_t idr = 0;
    if (t->core > 255u)
        return ESW_ERR_NOT_FOUND;
    return esw_ap_read(t->dap, (uint8_t)t->core, ESW_AP_IDR, &idr);
}

static esw_err_t generic_attach(esw_target_t *t)
{
    uint32_t idr = 0, cpuid = 0;
    t->nregions = 0;
    ESW_TRY(esw_ap_read(t->dap, (uint8_t)t->core, ESW_AP_IDR, &idr));
    esw_memap_init(&t->mem, t->dap, (uint8_t)t->core, ESW_CSW_CORTEXM_PROT);
    esw_cortexm_init(&t->cpu, &t->mem);
    if (AP_IDR_CLASS(idr) != AP_CLASS_MEM || esw_mem_read32(&t->mem, ESW_CM_CPUID, &cpuid) != ESW_OK) {
        (void)esw_dap_clear_errors(t->dap);
        t->locked = 1;   /* no memory access: still attached, for DAP-level access */
        snprintf(t->part, sizeof(t->part), "unknown (AP%u %08X)", (unsigned)t->core, (unsigned)idr);
        return ESW_OK;
    }
    snprintf(t->part, sizeof(t->part), "%s %08X", core_name(cpuid), (unsigned)cpuid);
    return ESW_OK;
}

static esw_err_t generic_unlock(esw_target_t *t, uint32_t flags)
{
    (void)flags;
    return t->locked ? ESW_ERR_UNSUPPORTED : ESW_OK;
}

static esw_err_t generic_reset(esw_target_t *t, uint32_t flags)
{
    if (t->locked)
        return ESW_ERR_LOCKED;
    if (!(flags & ESW_RESET_HALT))
        (void)t->cpu.ops->debug_off(&t->cpu);
    return t->cpu.ops->sys_reset(&t->cpu, (flags & ESW_RESET_HALT) ? 1u : 0u);
}

static esw_err_t generic_detach(esw_target_t *t)
{
    return t->locked ? ESW_OK : t->cpu.ops->debug_off(&t->cpu);
}

const esw_target_driver_t esw_target_generic = {
    .name = "generic",
    .probe = generic_probe,
    .attach = generic_attach,
    .unlock = generic_unlock,
    .erase = NULL,
    .program = NULL,
    .reset = generic_reset,
    .detach = generic_detach,
};

/* SPDX-License-Identifier: MIT */
/* nRF52 target module. Sequences proven on hardware (r5 + nRF52840 DK, build code F, 2026-11-06):
 * CTRL-AP ERASEALL unlock without reset, NVMC programming, UICR.APPROTECT = HwDisabled, CTRL-AP reset. */
#include "esw/targets/nrf52.h"
#include "esw/cortexm.h"
#include <stdio.h>
#include <string.h>

#define FICR_CODEPAGESIZE 0x10000010u
#define FICR_DEVICEID0    0x10000060u
#define FICR_INFO_PART    0x10000100u
#define NVMC_ERASEUICR    0x4001E514u
#define NVMC_ERASEALL     0x4001E50Cu
#define UICR_USED_WORDS   196u          /* 0x10001000..0x1000130F: NRFFW, NRFHW, CUSTOMER, PSELRESET .. REGOUT0 */
#define UICR_BASE         0x10001000u
#define UICR_SIZE         0x1000u
#define RAM_BASE          0x20000000u

#define NVMC_CFG_REN 0u
#define NVMC_CFG_WEN 1u
#define NVMC_CFG_EEN 2u

static esw_err_t ctrl_read(esw_target_t *t, uint8_t reg, uint32_t *v)
{
    return esw_ap_read(t->dap, ESW_NRF52_CTRL_AP, reg, v);
}

static esw_err_t ctrl_write(esw_target_t *t, uint8_t reg, uint32_t v)
{
    return esw_ap_write(t->dap, ESW_NRF52_CTRL_AP, reg, v);
}

esw_err_t esw_nrf52_approtect_open(esw_target_t *t, uint32_t *open)
{
    uint32_t v = 0;
    ESW_TRY(ctrl_read(t, ESW_NRF_CTRLAP_APPROTECTSTATUS, &v));
    *open = v & 1u;
    return ESW_OK;
}

esw_err_t esw_nrf52_erase_all(esw_target_t *t, uint32_t timeout_ms)
{
    ESW_TRY(ctrl_write(t, ESW_NRF_CTRLAP_ERASEALL, 1));
    uint32_t t0 = esw_now_ms(t->clk), busy = 1;
    while (busy) {
        ESW_TRY(ctrl_read(t, ESW_NRF_CTRLAP_ERASEALLSTATUS, &busy));
        busy &= 1u;
        if (busy && esw_elapsed_ms(t->clk, t0) >= timeout_ms)
            return ESW_ERR_TIMEOUT;
        if (busy)
            esw_sleep_ms(t->clk, 10);
    }
    ESW_TRY(ctrl_write(t, ESW_NRF_CTRLAP_ERASEALL, 0));
    esw_memap_invalidate(&t->mem);
    return ESW_OK;
}

esw_err_t esw_nrf52_ctrlap_reset(esw_target_t *t)
{
    ESW_TRY(ctrl_write(t, ESW_NRF_CTRLAP_RESET, 1));
    esw_sleep_ms(t->clk, 5);
    ESW_TRY(ctrl_write(t, ESW_NRF_CTRLAP_RESET, 0));
    esw_memap_invalidate(&t->mem);
    return ESW_OK;
}

esw_err_t esw_nrf52_read_info(esw_target_t *t, esw_nrf52_info_t *info)
{
    uint32_t w[5], pg[2];
    ESW_TRY(esw_mem_read_block(&t->mem, FICR_INFO_PART, w, 5));
    ESW_TRY(esw_mem_read_block(&t->mem, FICR_CODEPAGESIZE, pg, 2));
    ESW_TRY(esw_mem_read_block(&t->mem, FICR_DEVICEID0, info->deviceid, 2));
    info->part = w[0];
    info->variant = w[1];
    info->package = w[2];
    info->ram_kb = w[3];
    info->flash_kb = w[4];
    info->page_size = pg[0];
    info->page_count = pg[1];
    return ESW_OK;
}

static esw_err_t nvmc_wait(esw_target_t *t, uint32_t timeout_ms)
{
    uint32_t t0 = esw_now_ms(t->clk), ready = 0;
    for (;;) {
        ESW_TRY(esw_mem_read32(&t->mem, ESW_NRF_NVMC_READY, &ready));
        if (ready & 1u)
            return ESW_OK;
        if (esw_elapsed_ms(t->clk, t0) >= timeout_ms)
            return ESW_ERR_TIMEOUT;
        esw_sleep_ms(t->clk, 1);
    }
}

/* Bulk flash programming: one auto-increment MEM-AP write per word. While the NVMC is busy the AHB stalls the
 * access and the DP answers WAIT, which the ADIv5 layer retries, so the link runs at flash speed (tWRITE 41 us)
 * without a READY poll per word. Verified by read-back on real hardware (nRF52840, 2026-11-06). */
static esw_err_t nvmc_write_fast(esw_target_t *t, uint32_t addr, const uint32_t *words, uint32_t nwords)
{
    ESW_TRY(esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_WEN));
    esw_err_t e = esw_mem_write_block(&t->mem, addr, words, nwords);
    if (e == ESW_OK)
        e = nvmc_wait(t, 10);
    esw_err_t e2 = esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_REN);
    return e != ESW_OK ? e : e2;
}

/* Erase the whole flash with one NVMC ERASEALL (tERASEALL 169 ms instead of 85 ms per page). ERASEALL also erases
 * UICR, so the used UICR words are saved and written back: same end state as erasing every page. */
static esw_err_t erase_whole_flash(esw_target_t *t)
{
    uint32_t uicr[UICR_USED_WORDS];
    ESW_TRY(esw_mem_read_block(&t->mem, UICR_BASE, uicr, UICR_USED_WORDS));
    ESW_TRY(esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_EEN));
    esw_err_t e = esw_mem_write32(&t->mem, NVMC_ERASEALL, 1);
    if (e == ESW_OK)
        e = nvmc_wait(t, 1000);
    esw_err_t e2 = esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_REN);
    ESW_TRY(e != ESW_OK ? e : e2);
    /* APPROTECT first: if the restore is ever cut short, debug access must survive the next reset */
    const uint32_t ap = (ESW_NRF_UICR_APPROTECT - UICR_BASE) / 4u;
    if (uicr[ap] != 0xFFFFFFFFu)
        ESW_TRY(esw_nrf52_nvmc_write(t, ESW_NRF_UICR_APPROTECT, &uicr[ap], 1));
    for (uint32_t i = 0; i < UICR_USED_WORDS; i++)
        if (i != ap && uicr[i] != 0xFFFFFFFFu)
            ESW_TRY(esw_nrf52_nvmc_write(t, UICR_BASE + 4u * i, &uicr[i], 1));
    return ESW_OK;
}

esw_err_t esw_nrf52_nvmc_write(esw_target_t *t, uint32_t addr, const uint32_t *words, uint32_t nwords)
{
    ESW_TRY(esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_WEN));
    esw_err_t e = ESW_OK;
    for (uint32_t i = 0; i < nwords && e == ESW_OK; i++) {
        e = esw_mem_write32(&t->mem, addr + 4u * i, words[i]);
        if (e == ESW_OK)   /* tWRITE is 41 us: a few polls at most, no sleep in the common case */
            e = esw_mem_poll32(&t->mem, ESW_NRF_NVMC_READY, 1u, 1u, 1000, NULL);
    }
    esw_err_t e2 = esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_REN);
    return e != ESW_OK ? e : e2;
}

/* ---- driver ------------------------------------------------------------------------------------------- */

static esw_err_t nrf52_probe(esw_target_t *t)
{
    uint32_t idr = 0;
    if (t->dap->dpidr != ESW_NRF52_DPIDR)
        return ESW_ERR_NOT_FOUND;
    ESW_TRY(esw_ap_read(t->dap, ESW_NRF52_CTRL_AP, ESW_AP_IDR, &idr));
    return idr == ESW_NRF52_CTRL_AP_IDR ? ESW_OK : ESW_ERR_NOT_FOUND;
}

static esw_err_t nrf52_attach(esw_target_t *t)
{
    uint32_t open = 0;
    esw_memap_init(&t->mem, t->dap, ESW_NRF52_AHB_AP, ESW_CSW_CORTEXM_PROT);
    esw_cortexm_init(&t->cpu, &t->mem);
    t->nregions = 0;
    ESW_TRY(esw_nrf52_approtect_open(t, &open));
    t->locked = !open;
    if (t->locked) {
        snprintf(t->part, sizeof(t->part), "nRF52 (APPROTECT)");
        return ESW_OK;
    }
    esw_nrf52_info_t info;
    ESW_TRY(esw_nrf52_read_info(t, &info));
    char var[5];
    for (int i = 0; i < 4; i++) {
        char ch = (char)((info.variant >> (24 - 8 * i)) & 0xFFu);
        var[i] = (ch >= 32 && ch < 127) ? ch : '?';
    }
    var[4] = 0;
    snprintf(t->part, sizeof(t->part), "nRF%05X-%s", (unsigned)(info.part & 0xFFFFFu), var);
    t->scratch_ram = RAM_BASE;   /* 4 KB in RAM0: CRC loader code + table + stack (pyOCD uses the same area) */
    const esw_mem_region_t flash = { "flash", ESW_MEM_FLASH, 0, info.flash_kb * 1024u, info.page_size, NULL, 0 };
    const esw_mem_region_t uicr = { "uicr", ESW_MEM_OTP, UICR_BASE, UICR_SIZE, UICR_SIZE, NULL, 0 };
    const esw_mem_region_t ram = { "ram", ESW_MEM_RAM, RAM_BASE, info.ram_kb * 1024u, 4, NULL, 0 };
    ESW_TRY(esw_target_add_region(t, &flash));
    ESW_TRY(esw_target_add_region(t, &uicr));
    return esw_target_add_region(t, &ram);
}

static esw_err_t nrf52_unlock(esw_target_t *t, uint32_t flags)
{
    if (!t->locked)
        return ESW_OK;
    if (!(flags & ESW_UNLOCK_ALLOW_ERASE))
        return ESW_ERR_LOCKED;
    ESW_TRY(esw_nrf52_erase_all(t, 2000));
    /* ERASEALL also erased UICR: until UICR.APPROTECT = HwDisabled is written below, a reset would re-engage the
     * hardware APPROTECT of build code F. So: no reset in between. */
    ESW_TRY(nrf52_attach(t));
    if (t->locked)
        return ESW_ERR_LOCKED;
    return esw_nrf52_nvmc_write(t, ESW_NRF_UICR_APPROTECT, (const uint32_t[]){ ESW_NRF_APPROTECT_HWDISABLED }, 1);
}

static esw_err_t nrf52_erase(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, uint32_t len)
{
    ESW_TRY(t->cpu.ops->halt(&t->cpu));
    if (r->kind == ESW_MEM_RAM)
        return ESW_OK;
    if (r->kind == ESW_MEM_OTP) {   /* UICR erases as a whole */
        if (addr != r->start || len != r->size)
            return ESW_ERR_UNSUPPORTED;
        ESW_TRY(esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_EEN));
        ESW_TRY(esw_mem_write32(&t->mem, NVMC_ERASEUICR, 1));
        ESW_TRY(nvmc_wait(t, 200));
        return esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_REN);
    }
    if (r->kind == ESW_MEM_FLASH && addr == r->start && len == r->size)
        return erase_whole_flash(t);
    ESW_TRY(esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_EEN));
    esw_err_t e = ESW_OK;
    for (uint32_t a = addr - (addr % r->sector_size); a < addr + len && e == ESW_OK; a += r->sector_size) {
        e = esw_mem_write32(&t->mem, ESW_NRF_NVMC_ERASEPAGE, a);
        if (e == ESW_OK)
            e = nvmc_wait(t, 200);   /* tERASEPAGE 85 ms */
    }
    esw_err_t e2 = esw_mem_write32(&t->mem, ESW_NRF_NVMC_CONFIG, NVMC_CFG_REN);
    return e != ESW_OK ? e : e2;
}

static esw_err_t nrf52_program(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, const uint32_t *data,
                               uint32_t nwords)
{
    ESW_TRY(t->cpu.ops->halt(&t->cpu));
    if (r->kind == ESW_MEM_RAM)
        return esw_mem_write_block(&t->mem, addr, data, nwords);
    if (r->kind == ESW_MEM_FLASH)
        return nvmc_write_fast(t, addr, data, nwords);
    return esw_nrf52_nvmc_write(t, addr, data, nwords);   /* UICR: word by word with READY polling */
}

static esw_err_t nrf52_reset(esw_target_t *t, uint32_t flags)
{
    esw_err_t e;
    if (flags & ESW_RESET_HALT) {
        e = t->cpu.ops->sys_reset(&t->cpu, 1);
    } else {
        if (!t->locked)
            (void)t->cpu.ops->debug_off(&t->cpu);   /* AHB-AP faults while locked; the CTRL-AP reset still works */
        e = esw_nrf52_ctrlap_reset(t);   /* works without UICR.PSELRESET */
    }
    /* Build code F engages APPROTECT at reset when UICR.APPROTECT is not HwDisabled, and running firmware may lock
     * it too (a random image did on 2026-11-06): report that as LOCKED, not as the bus fault it causes. The CTRL-AP
     * stays reachable either way, so unlock (mass erase) always recovers. */
    uint32_t open = 0;
    (void)esw_dap_clear_errors(t->dap);
    ESW_TRY(esw_nrf52_approtect_open(t, &open));
    if (!open) {
        t->locked = 1;
        return ESW_ERR_LOCKED;
    }
    return e;
}

static esw_err_t nrf52_detach(esw_target_t *t)
{
    if (t->locked)
        return ESW_OK;
    return t->cpu.ops->debug_off(&t->cpu);
}

const esw_target_driver_t esw_target_nrf52 = {
    .name = "nrf52",
    .probe = nrf52_probe,
    .attach = nrf52_attach,
    .unlock = nrf52_unlock,
    .erase = nrf52_erase,
    .program = nrf52_program,
    .reset = nrf52_reset,
    .detach = nrf52_detach,
};

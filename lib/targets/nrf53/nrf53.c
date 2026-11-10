/* SPDX-License-Identifier: MIT */
/* nRF5340 target module (application and network core). Register layout from Nordic's MDK (nrfx bsp/stable/mdk/
 * nrf53): the NVMC has no ERASEPAGE register (a page is erased by writing 0xFFFFFFFF into it with CONFIG = Een) and
 * no UICR erase (only ERASEALL, which erases the whole flash too). */
#include "esw/targets/nrf53.h"
#include "esw/cortexm.h"
#include <stdio.h>

#define NVMC_READY    0x400u
#define NVMC_CONFIG   0x504u
#define NVMC_ERASEALL 0x50Cu
#define KMU_SELECTKEYSLOT 0x500u   /* KMU shares the NVMC's base (0x50039000), registers do not overlap */
#define UICR_KEYSLOT_CONFIG 0x400u /* 128 x {DEST, PERM} */
#define UICR_KEYSLOT_KEY    0x800u /* 128 x 4 words */
#define UICR_DATA_WORDS     256u   /* 0x000..0x3FF: APPROTECT ... OTP: everything outside the key slots */
#define NVMC_CFG_REN 0u
#define NVMC_CFG_WEN 1u
#define NVMC_CFG_EEN 2u

#define FICR_PART         0x20Cu
#define FICR_VARIANT      0x210u
#define FICR_RAM          0x218u
#define FICR_FLASH        0x21Cu
#define FICR_CODEPAGESIZE 0x220u

#define UICR_SIZE_APP 0x1000u
#define UICR_SIZE_NET 0x380u

#define P_NVMC     0   /* priv: NVMC base of the attached core */
#define P_FORCEOFF 1   /* 1: attach released RESET.NETWORK.FORCEOFF; detach restores it */
#define P_CLEAN    2   /* 1: reset-halted once in this session, no firmware has run since */

/* Flash work starts from a reset-halt, once per session: the running firmware may have locked flash regions in the
 * SPU (MCUboot/TF-M write-protect the bootloader: FLASHREGION[0].PERM = 0x115 seen on a live DUT, 2026-11-10), and a
 * halt alone keeps those locks. After the reset nothing of the firmware has run, so the SPU is at its defaults. */
static esw_err_t flash_prepare(esw_target_t *t)
{
    if (t->priv[P_CLEAN])
        return t->cpu.ops->halt(&t->cpu);
    ESW_TRY(t->cpu.ops->sys_reset(&t->cpu, 1));
    t->priv[P_CLEAN] = 1;
    return ESW_OK;
}

static uint8_t ctrl_ap(const esw_target_t *t)
{
    return (uint8_t)ESW_NRF53_CTRL_AP(t->core);
}

static esw_err_t approtect_open(esw_target_t *t, uint32_t *open)
{
    uint32_t v = 0;
    ESW_TRY(esw_ap_read(t->dap, ctrl_ap(t), ESW_NRF53_CTRLAP_APPROTECTSTATUS, &v));
    /* application core: the MEM-AP accesses are secure, so both APPROTECT and SECUREAPPROTECT must be off */
    uint32_t need = t->core == ESW_NRF53_CORE_APP ? 3u : 1u;
    *open = (v & need) == need;
    return ESW_OK;
}

esw_err_t esw_nrf53_erase_all(esw_target_t *t, uint32_t timeout_ms)
{
    ESW_TRY(esw_ap_write(t->dap, ctrl_ap(t), ESW_NRF53_CTRLAP_ERASEALL, 1));
    uint32_t t0 = esw_now_ms(t->clk), busy = 1;
    while (busy) {
        ESW_TRY(esw_ap_read(t->dap, ctrl_ap(t), ESW_NRF53_CTRLAP_ERASEALLSTATUS, &busy));
        busy &= 1u;
        if (busy && esw_elapsed_ms(t->clk, t0) >= timeout_ms)
            return ESW_ERR_TIMEOUT;
        if (busy)
            esw_sleep_ms(t->clk, 10);
    }
    ESW_TRY(esw_ap_write(t->dap, ctrl_ap(t), ESW_NRF53_CTRLAP_ERASEALL, 0));
    esw_memap_invalidate(&t->mem);
    return ESW_OK;
}

esw_err_t esw_nrf53_ctrlap_reset(esw_target_t *t)
{
    ESW_TRY(esw_ap_write(t->dap, ctrl_ap(t), ESW_NRF53_CTRLAP_RESET, 1));
    esw_sleep_ms(t->clk, 5);
    ESW_TRY(esw_ap_write(t->dap, ctrl_ap(t), ESW_NRF53_CTRLAP_RESET, 0));
    esw_memap_invalidate(&t->mem);
    return ESW_OK;
}

static esw_err_t nvmc_config(esw_target_t *t, uint32_t cfg)
{
    return esw_mem_write32(&t->mem, t->priv[P_NVMC] + NVMC_CONFIG, cfg);
}

static esw_err_t nvmc_wait(esw_target_t *t, uint32_t timeout_ms)
{
    uint32_t t0 = esw_now_ms(t->clk), ready = 0;
    for (;;) {
        ESW_TRY(esw_mem_read32(&t->mem, t->priv[P_NVMC] + NVMC_READY, &ready));
        if (ready & 1u)
            return ESW_OK;
        if (esw_elapsed_ms(t->clk, t0) >= timeout_ms)
            return ESW_ERR_TIMEOUT;
        esw_sleep_ms(t->clk, 1);
    }
}

/* Word by word with a READY poll after each (UICR, and the reference path for flash). */
static esw_err_t nvmc_write_words(esw_target_t *t, uint32_t addr, const uint32_t *w, uint32_t n)
{
    ESW_TRY(nvmc_config(t, NVMC_CFG_WEN));
    esw_err_t e = ESW_OK;
    for (uint32_t i = 0; i < n && e == ESW_OK; i++) {
        e = esw_mem_write32(&t->mem, addr + 4u * i, w[i]);
        if (e == ESW_OK)
            e = esw_mem_poll32(&t->mem, t->priv[P_NVMC] + NVMC_READY, 1u, 1u, 1000, NULL);
    }
    esw_err_t e2 = nvmc_config(t, NVMC_CFG_REN);
    return e != ESW_OK ? e : e2;
}

/* Flash: one auto-increment block write. While the NVMC is busy the AHB stalls and the DP answers WAIT, which the
 * ADIv5 layer retries (the nRF52's proven method); a READY wait closes the block. */
static esw_err_t nvmc_write_block(esw_target_t *t, uint32_t addr, const uint32_t *w, uint32_t n)
{
    ESW_TRY(nvmc_config(t, NVMC_CFG_WEN));
    esw_err_t e = esw_mem_write_block(&t->mem, addr, w, n);
    if (e == ESW_OK)
        e = nvmc_wait(t, 10);
    esw_err_t e2 = nvmc_config(t, NVMC_CFG_REN);
    return e != ESW_OK ? e : e2;
}

/* ---- driver ------------------------------------------------------------------------------------------- */

static esw_err_t nrf53_probe(esw_target_t *t)
{
    uint32_t a = 0, n = 0;
    if (t->dap->dpidr != ESW_NRF53_DPIDR)
        return ESW_ERR_NOT_FOUND;
    ESW_TRY(esw_ap_read(t->dap, ESW_NRF53_CTRL_AP(0), ESW_AP_IDR, &a));
    ESW_TRY(esw_ap_read(t->dap, ESW_NRF53_CTRL_AP(1), ESW_AP_IDR, &n));
    return (a == ESW_NRF53_CTRL_AP_IDR && n == ESW_NRF53_CTRL_AP_IDR) ? ESW_OK : ESW_ERR_NOT_FOUND;
}

/* The network core's AHB-AP answers only while the core is not held off. */
static void release_network(esw_target_t *t)
{
    esw_memap_t app;
    uint32_t v = 0, open = 0, st = 0;
    if (esw_ap_read(t->dap, ESW_NRF53_CTRL_AP(0), ESW_NRF53_CTRLAP_APPROTECTSTATUS, &st) != ESW_OK)
        return;
    open = (st & 3u) == 3u;
    if (!open)
        return;   /* application core locked: cannot touch RESET.NETWORK */
    esw_memap_init(&app, t->dap, 0, ESW_CSW_CORTEXM_PROT);
    if (esw_mem_read32(&app, ESW_NRF53_NET_FORCEOFF, &v) == ESW_OK && (v & 1u) &&
        esw_mem_write32(&app, ESW_NRF53_NET_FORCEOFF, 0) == ESW_OK) {
        t->priv[P_FORCEOFF] = 1;
        esw_sleep_ms(t->clk, 5);
    }
    (void)esw_dap_clear_errors(t->dap);
}

static esw_err_t nrf53_attach(esw_target_t *t)
{
    uint32_t open = 0, w[6];
    if (t->core > ESW_NRF53_CORE_NET)
        return ESW_ERR_ARG;
    const uint32_t net = t->core == ESW_NRF53_CORE_NET;
    if (net && !t->priv[P_FORCEOFF])
        release_network(t);
    esw_memap_init(&t->mem, t->dap, (uint8_t)t->core, ESW_CSW_CORTEXM_PROT);
    esw_cortexm_init(&t->cpu, &t->mem);
    t->nregions = 0;
    t->priv[P_NVMC] = net ? ESW_NRF53_NET_NVMC : ESW_NRF53_APP_NVMC;
    ESW_TRY(approtect_open(t, &open));
    /* Two branches must both be open. The UICR branch shows in the CTRL-AP's APPROTECT.STATUS. The firmware branch
     * closes at every reset until the firmware's startup code writes CTRLAP.APPROTECT.DISABLE (Nordic's
     * nrf53_handle_approtect()); it shows only as the AHB-AP's CSW.DeviceEn = 0. An image without that startup code
     * (or a random one: 2026-11-10) locks the debugger out after the next pin reset; only ERASEALL recovers. */
    uint32_t csw = 0, fw_closed = 0;
    if (open && esw_ap_read(t->dap, (uint8_t)t->core, ESW_AP_CSW, &csw) == ESW_OK)
        fw_closed = !(csw & ESW_CSW_DEVICEEN);
    t->locked = !open || fw_closed;
    if (t->locked) {
        snprintf(t->part, sizeof(t->part), "nRF5340 %s (APPROTECT%s)", net ? "net" : "app", fw_closed ? " fw" : "");
        return ESW_OK;
    }
    const uint32_t ficr = net ? ESW_NRF53_NET_FICR : ESW_NRF53_APP_FICR;
    ESW_TRY(esw_mem_read_block(&t->mem, ficr + FICR_PART, w, 6));   /* PART VARIANT PACKAGE RAM FLASH PAGESIZE */
    char var[5];
    for (int i = 0; i < 4; i++) {
        char ch = (char)((w[1] >> (24 - 8 * i)) & 0xFFu);
        var[i] = (ch >= 32 && ch < 127) ? ch : '?';
    }
    var[4] = 0;
    snprintf(t->part, sizeof(t->part), "nRF%04X-%s %s", (unsigned)(w[0] & 0xFFFFu), var, net ? "net" : "app");
    const uint32_t flash_base = net ? ESW_NRF53_NET_FLASH : 0u, ram = net ? ESW_NRF53_NET_RAM : ESW_NRF53_APP_RAM;
    t->scratch_ram = ram;   /* 4 KB at the start of RAM for the CRC loader */
    const esw_mem_region_t flash = { "flash", ESW_MEM_FLASH, flash_base, w[4] * 1024u, w[5], NULL, 0 };
    const esw_mem_region_t uicr = { "uicr", ESW_MEM_OTP, net ? ESW_NRF53_NET_UICR : ESW_NRF53_APP_UICR,
                                    net ? UICR_SIZE_NET : UICR_SIZE_APP, net ? UICR_SIZE_NET : UICR_SIZE_APP, NULL, 0 };
    const esw_mem_region_t rram = { "ram", ESW_MEM_RAM, ram, w[3] * 1024u, 4, NULL, 0 };
    ESW_TRY(esw_target_add_region(t, &flash));
    ESW_TRY(esw_target_add_region(t, &uicr));
    return esw_target_add_region(t, &rram);
}

static esw_err_t nrf53_unlock(esw_target_t *t, uint32_t flags)
{
    if (!t->locked)
        return ESW_OK;
    if (!(flags & ESW_UNLOCK_ALLOW_ERASE))
        return ESW_ERR_LOCKED;
    ESW_TRY(esw_nrf53_erase_all(t, 3000));
    /* ERASEALL erased UICR too: write UICR.(SECURE)APPROTECT = Unprotected before any reset, or the hardware
     * APPROTECT engages again at the next reset */
    ESW_TRY(nrf53_attach(t));
    if (t->locked)
        return ESW_ERR_LOCKED;
    const uint32_t uicr = t->core == ESW_NRF53_CORE_NET ? ESW_NRF53_NET_UICR : ESW_NRF53_APP_UICR;
    const uint32_t un = ESW_NRF53_APPROTECT_UNPROTECTED;
    ESW_TRY(t->cpu.ops->halt(&t->cpu));   /* blank flash: the core sits in lockup; no reset from here on */
    ESW_TRY(nvmc_write_words(t, uicr + ESW_NRF53_UICR_APPROTECT, &un, 1));
    if (t->core == ESW_NRF53_CORE_APP)
        ESW_TRY(nvmc_write_words(t, uicr + ESW_NRF53_UICR_SECUREAPPROTECT, &un, 1));
    /* the firmware branch stays open only until the next reset: flash work must not reset-halt first */
    t->priv[P_CLEAN] = 1;
    return ESW_OK;
}

/* Every KMU key slot empty? A slot's key words read 0xDEADDEAD unless the KMU has it selected, so each slot is
 * selected and read: empty only if CONFIG and all four key words read erased. Unreadable (read-protected or
 * revoked) counts as in use. */
static esw_err_t kmu_slots_empty(esw_target_t *t, uint32_t *empty)
{
    const uint32_t sel = ESW_NRF53_APP_NVMC + KMU_SELECTKEYSLOT;
    esw_err_t e = ESW_OK;
    *empty = 1;
    for (uint32_t n = 1; n <= 128u && e == ESW_OK && *empty; n++) {
        uint32_t c[2] = { 0, 0 }, k[4] = { 0, 0, 0, 0 };
        e = esw_mem_write32(&t->mem, sel, n);
        if (e == ESW_OK)
            e = esw_mem_read_block(&t->mem, ESW_NRF53_APP_UICR + UICR_KEYSLOT_CONFIG + 8u * (n - 1u), c, 2);
        if (e == ESW_OK)
            e = esw_mem_read_block(&t->mem, ESW_NRF53_APP_UICR + UICR_KEYSLOT_KEY + 16u * (n - 1u), k, 4);
        if ((c[0] & c[1] & k[0] & k[1] & k[2] & k[3]) != 0xFFFFFFFFu)
            *empty = 0;
    }
    esw_err_t e2 = esw_mem_write32(&t->mem, sel, 0);
    return e != ESW_OK ? e : e2;
}

/* Whole flash with one NVMC ERASEALL (0.2 s instead of 256 x 87 ms page erases). ERASEALL also erases UICR, so its
 * data words are saved and written back (APPROTECT first: if the restore is cut short, debug access must survive
 * the next reset). The KMU key slots cannot be saved through the debugger, so on the application core this path
 * runs only when every slot is provably empty; otherwise the caller erases page by page. */
static esw_err_t erase_whole_flash(esw_target_t *t, uint32_t *done)
{
    const uint32_t net = t->core == ESW_NRF53_CORE_NET;
    const uint32_t ubase = net ? ESW_NRF53_NET_UICR : ESW_NRF53_APP_UICR;
    const uint32_t nw = net ? UICR_SIZE_NET / 4u : UICR_DATA_WORDS;
    uint32_t uicr[UICR_DATA_WORDS], empty = 1;
    *done = 0;
    if (!net)
        ESW_TRY(kmu_slots_empty(t, &empty));
    if (!empty)
        return ESW_OK;
    ESW_TRY(esw_mem_read_block(&t->mem, ubase, uicr, nw));
    ESW_TRY(nvmc_config(t, NVMC_CFG_EEN));
    esw_err_t e = esw_mem_write32(&t->mem, t->priv[P_NVMC] + NVMC_ERASEALL, 1);
    if (e == ESW_OK)
        e = nvmc_wait(t, 2000);
    esw_err_t e2 = nvmc_config(t, NVMC_CFG_REN);
    ESW_TRY(e != ESW_OK ? e : e2);
    const uint32_t ap = ESW_NRF53_UICR_APPROTECT / 4u, sap = ESW_NRF53_UICR_SECUREAPPROTECT / 4u;
    if (uicr[ap] != 0xFFFFFFFFu)
        ESW_TRY(nvmc_write_words(t, ubase + 4u * ap, &uicr[ap], 1));
    if (!net && uicr[sap] != 0xFFFFFFFFu)
        ESW_TRY(nvmc_write_words(t, ubase + 4u * sap, &uicr[sap], 1));
    for (uint32_t i = 0; i < nw; i++)
        if (i != ap && (net || i != sap) && uicr[i] != 0xFFFFFFFFu)
            ESW_TRY(nvmc_write_words(t, ubase + 4u * i, &uicr[i], 1));
    *done = 1;
    return ESW_OK;
}

static esw_err_t nrf53_erase(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, uint32_t len)
{
    if (r->kind == ESW_MEM_RAM)
        return t->cpu.ops->halt(&t->cpu);
    ESW_TRY(flash_prepare(t));
    if (r->kind != ESW_MEM_FLASH)
        return ESW_ERR_UNSUPPORTED;   /* UICR: only ERASEALL (esw_nrf53_erase_all) erases it */
    if (addr == r->start && len == r->size) {
        uint32_t done = 0;
        ESW_TRY(erase_whole_flash(t, &done));
        if (done)
            return ESW_OK;
    }
    ESW_TRY(nvmc_config(t, NVMC_CFG_EEN));
    esw_err_t e = ESW_OK;
    for (uint32_t a = addr - (addr - r->start) % r->sector_size; a < addr + len && e == ESW_OK; a += r->sector_size) {
        e = esw_mem_write32(&t->mem, a, 0xFFFFFFFFu);   /* starts the page erase */
        if (e == ESW_OK)
            e = nvmc_wait(t, 200);
    }
    esw_err_t e2 = nvmc_config(t, NVMC_CFG_REN);
    return e != ESW_OK ? e : e2;
}

static esw_err_t nrf53_program(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, const uint32_t *data,
                               uint32_t nwords)
{
    if (r->kind == ESW_MEM_RAM) {
        ESW_TRY(t->cpu.ops->halt(&t->cpu));
        return esw_mem_write_block(&t->mem, addr, data, nwords);
    }
    ESW_TRY(flash_prepare(t));
    if (r->kind == ESW_MEM_FLASH)
        return nvmc_write_block(t, addr, data, nwords);
    return nvmc_write_words(t, addr, data, nwords);
}

static esw_err_t nrf53_reset(esw_target_t *t, uint32_t flags)
{
    esw_err_t e;
    t->priv[P_CLEAN] = (flags & ESW_RESET_HALT) ? 1u : 0u;
    if (flags & ESW_RESET_HALT) {
        e = t->cpu.ops->sys_reset(&t->cpu, 1);
    } else {
        if (!t->locked)
            (void)t->cpu.ops->debug_off(&t->cpu);
        e = esw_nrf53_ctrlap_reset(t);
    }
    /* as on the nRF52: a reset re-engages APPROTECT unless UICR says Unprotected and the firmware opens it */
    uint32_t open = 0;
    (void)esw_dap_clear_errors(t->dap);
    ESW_TRY(approtect_open(t, &open));
    if (!open) {
        t->locked = 1;
        return ESW_ERR_LOCKED;
    }
    return e;
}

static esw_err_t nrf53_detach(esw_target_t *t)
{
    esw_err_t e = t->locked ? ESW_OK : t->cpu.ops->debug_off(&t->cpu);
    if (t->priv[P_FORCEOFF]) {   /* leave the network core as found: held off */
        esw_memap_t app;
        esw_memap_init(&app, t->dap, 0, ESW_CSW_CORTEXM_PROT);
        (void)esw_mem_write32(&app, ESW_NRF53_NET_FORCEOFF, 1);
        t->priv[P_FORCEOFF] = 0;
    }
    return e;
}

const esw_target_driver_t esw_target_nrf53 = {
    .name = "nrf53",
    .probe = nrf53_probe,
    .attach = nrf53_attach,
    .unlock = nrf53_unlock,
    .erase = nrf53_erase,
    .program = nrf53_program,
    .reset = nrf53_reset,
    .detach = nrf53_detach,
};

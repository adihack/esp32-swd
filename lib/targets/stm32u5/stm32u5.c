/* SPDX-License-Identifier: MIT */
/* STM32U5 target module: internal flash through the FLASH non-secure registers (TrustZone off).
 *
 * Programming unit is a quad-word (16 bytes, 16-byte aligned); every quad-word may be written once between erases
 * (ECC). Erased quad-words of an image (all 0xFF) are skipped, so they stay erased. Pages are 8 KB; the 2 MB and
 * 4 MB parts are always dual-bank, the smaller ones by the DUALBANK option. SWAP_BANK swaps the banks' addresses,
 * so the physical bank of an address is (offset >= bank size) XOR SWAP_BANK.
 *
 * Not yet: TrustZone (TZEN = 1: secure registers and SECBB handling), option-byte programming other than the RDP
 * regression in unlock(), the external OCTOSPI flash (needs a loader). */
#include "esw/targets/stm32u5.h"
#include "esw/cortexm.h"
#include <stdio.h>

#define REG(off) (ESW_U5_FLASH_NS + (off))

enum {
    P_DEVID,      /* DBGMCU DEV_ID */
    P_OPTR,       /* FLASH_OPTR at attach */
    P_BANK_SIZE,  /* bytes */
    P_DUAL,
    P_FZR1,       /* DBGMCU_APB1FZR1 as found (restored at detach) */
    P_FZR1_SET,   /* 1: attach changed it */
    P_CLEAN,      /* 1: reset-halted once in this session, no firmware has run since */
};

/* Flash work starts from a reset-halt, once per session, so nothing the firmware set up at run time (DMA, a started
 * watchdog, flash or MPU settings) is active. */
static esw_err_t flash_prepare(esw_target_t *t)
{
    if (t->priv[P_CLEAN])
        return t->cpu.ops->halt(&t->cpu);
    ESW_TRY(t->cpu.ops->sys_reset(&t->cpu, 1));
    t->priv[P_CLEAN] = 1;
    return ESW_OK;
}

static uint32_t rdp_of(uint32_t optr)
{
    return optr & ESW_U5_OPTR_RDP_MASK;
}

static esw_err_t sr_read(esw_target_t *t, uint32_t *sr)
{
    return esw_mem_read32(&t->mem, REG(ESW_U5_FLASH_NSSR), sr);
}

/* Wait until the flash is idle (BSY and WDW clear), then clear the error flags; report them if asked (stale flags
 * from an earlier operation are cleared without failing). */
static esw_err_t flash_idle(esw_target_t *t, uint32_t timeout_ms, uint32_t report)
{
    uint32_t t0 = esw_now_ms(t->clk), sr = 0;
    for (uint32_t i = 0;; i++) {
        ESW_TRY(sr_read(t, &sr));
        if (!(sr & (ESW_U5_SR_BSY | ESW_U5_SR_WDW)))
            break;
        if (esw_elapsed_ms(t->clk, t0) >= timeout_ms)
            return ESW_ERR_TIMEOUT;
        if (i >= 16)   /* quad-word program ~0.1 ms: poll fast first, then yield */
            esw_sleep_ms(t->clk, 1);
    }
    if (sr & (ESW_U5_SR_ERRORS | ESW_U5_SR_EOP))
        ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSSR), sr & (ESW_U5_SR_ERRORS | ESW_U5_SR_EOP)));
    if (!report)
        return ESW_OK;
    if (sr & ESW_U5_SR_WRPERR)
        return ESW_ERR_LOCKED;   /* write-protected area */
    return (sr & ESW_U5_SR_ERRORS) ? ESW_ERR_FAULT : ESW_OK;
}

static esw_err_t flash_wait(esw_target_t *t, uint32_t timeout_ms)
{
    return flash_idle(t, timeout_ms, 1);
}

static esw_err_t flash_unlock(esw_target_t *t)
{
    uint32_t cr = 0;
    ESW_TRY(esw_mem_read32(&t->mem, REG(ESW_U5_FLASH_NSCR), &cr));
    if (cr & ESW_U5_CR_LOCK) {
        ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSKEYR), ESW_U5_KEY1));
        ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSKEYR), ESW_U5_KEY2));
        ESW_TRY(esw_mem_read32(&t->mem, REG(ESW_U5_FLASH_NSCR), &cr));
        if (cr & ESW_U5_CR_LOCK)
            return ESW_ERR_LOCKED;   /* wrong sequence locks NSCR until the next reset */
    }
    return flash_idle(t, 100, 0);   /* idle, stale error flags cleared */
}

static esw_err_t flash_lock(esw_target_t *t)
{
    return esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), ESW_U5_CR_LOCK);
}

/* The ICACHE also caches debugger reads of the flash: drop stale lines after erase or program. */
static void icache_invalidate(esw_target_t *t)
{
    uint32_t cr = 0;
    if (esw_mem_read32(&t->mem, ESW_U5_ICACHE_CR, &cr) == ESW_OK && (cr & 1u))
        (void)esw_mem_write32(&t->mem, ESW_U5_ICACHE_CR, cr | 2u);
}

static esw_err_t usable(esw_target_t *t)
{
    if (t->locked)
        return ESW_ERR_LOCKED;
    if (t->priv[P_OPTR] & ESW_U5_OPTR_TZEN)
        return ESW_ERR_UNSUPPORTED;
    return ESW_OK;
}

/* ---- driver ------------------------------------------------------------------------------------------- */

static esw_err_t u5_probe(esw_target_t *t)
{
    esw_memap_t m;
    uint32_t idr = 0, id = 0;
    if ((t->dap->dpidr & 0x0FFFu) != 0x0477u)   /* ARM-designed SW-DP */
        return ESW_ERR_NOT_FOUND;
    ESW_TRY(esw_ap_read(t->dap, 0, ESW_AP_IDR, &idr));
    if (((idr >> 13) & 0xFu) != 0x8u)   /* AP0 must be a MEM-AP */
        return ESW_ERR_NOT_FOUND;
    esw_memap_init(&m, t->dap, 0, ESW_CSW_CORTEXM_PROT);
    ESW_TRY(esw_mem_read32(&m, ESW_U5_DBGMCU_IDCODE, &id));
    id &= 0xFFFu;
    return (id == 0x482u || id == 0x481u) ? ESW_OK : ESW_ERR_NOT_FOUND;
}

static esw_err_t u5_attach(esw_target_t *t)
{
    uint32_t idcode = 0, optr = 0, fsz = 0;
    if (t->core != 0)
        return ESW_ERR_ARG;
    esw_memap_init(&t->mem, t->dap, 0, ESW_CSW_CORTEXM_PROT);
    esw_cortexm_init(&t->cpu, &t->mem);
    t->nregions = 0;
    ESW_TRY(esw_mem_read32(&t->mem, ESW_U5_DBGMCU_IDCODE, &idcode));
    ESW_TRY(esw_mem_read32(&t->mem, REG(ESW_U5_FLASH_OPTR), &optr));
    const uint32_t dev = idcode & 0xFFFu, rdp = rdp_of(optr);
    t->priv[P_DEVID] = dev;
    t->priv[P_OPTR] = optr;
    /* the flash size word sits in system memory: unreadable under RDP 1, then assume the family maximum */
    if (esw_mem_read32(&t->mem, ESW_U5_FLASHSIZE, &fsz) != ESW_OK || (fsz & 0xFFFFu) == 0xFFFFu || !(fsz & 0xFFFFu)) {
        (void)esw_dap_clear_errors(t->dap);
        fsz = dev == 0x481u ? 4096u : 2048u;
    }
    const uint32_t flash_kb = fsz & 0xFFFFu;
    const uint32_t dual = flash_kb >= 2048u || (optr & ESW_U5_OPTR_DUALBANK);
    t->priv[P_DUAL] = dual;
    t->priv[P_BANK_SIZE] = flash_kb * 1024u / (dual ? 2u : 1u);
    t->locked = rdp != ESW_U5_RDP_L0;
    const char *lvl = rdp == ESW_U5_RDP_L0     ? ""
                      : rdp == ESW_U5_RDP_L0_5 ? " RDP0.5"
                      : rdp == ESW_U5_RDP_L2   ? " RDP2"
                                               : " RDP1";
    snprintf(t->part, sizeof(t->part), "STM32U5 %03X %uK%s%s", (unsigned)dev, (unsigned)flash_kb,
             (optr & ESW_U5_OPTR_TZEN) ? " TZ" : "", lvl);
    if (t->locked)
        return ESW_OK;
    /* freeze the watchdogs while the core is halted (only matters while halted; restored at detach) */
    uint32_t fz = 0;
    if (esw_mem_read32(&t->mem, ESW_U5_DBGMCU_APB1FZR1, &fz) == ESW_OK) {
        t->priv[P_FZR1] = fz;
        const uint32_t want = fz | ESW_U5_FZR1_IWDG | ESW_U5_FZR1_WWDG;
        if (want != fz && esw_mem_write32(&t->mem, ESW_U5_DBGMCU_APB1FZR1, want) == ESW_OK)
            t->priv[P_FZR1_SET] = 1;
    }
    t->scratch_ram = ESW_U5_SRAM1;   /* 4 KB at the start of SRAM1 for the CRC loader */
    const esw_mem_region_t flash = { "flash", ESW_MEM_FLASH, ESW_U5_FLASH_BASE, flash_kb * 1024u, ESW_U5_PAGE_SIZE,
                                     NULL, 0 };
    const esw_mem_region_t ram = { "ram", ESW_MEM_RAM, ESW_U5_SRAM1, 256u * 1024u, 4, NULL, 0 };
    ESW_TRY(esw_target_add_region(t, &flash));
    return esw_target_add_region(t, &ram);
}

/* RDP level 1 (or 0.5) -> 0. DESTRUCTIVE: the regression mass-erases the flash. Level 2 is permanent. Untested on
 * hardware. */
static esw_err_t u5_unlock(esw_target_t *t, uint32_t flags)
{
    uint32_t sr = 0, cr = 0;
    if (!t->locked)
        return ESW_OK;
    if (rdp_of(t->priv[P_OPTR]) == ESW_U5_RDP_L2 || !(flags & ESW_UNLOCK_ALLOW_ERASE))
        return ESW_ERR_LOCKED;
    ESW_TRY(sr_read(t, &sr));
    if (sr & ESW_U5_SR_OEM1LOCK)
        return ESW_ERR_LOCKED;   /* regression needs the OEM1 key */
    ESW_TRY(flash_unlock(t));
    ESW_TRY(esw_mem_read32(&t->mem, REG(ESW_U5_FLASH_NSCR), &cr));
    if (cr & ESW_U5_CR_OPTLOCK) {
        ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_OPTKEYR), ESW_U5_OPTKEY1));
        ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_OPTKEYR), ESW_U5_OPTKEY2));
    }
    ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_OPTR),
                            (t->priv[P_OPTR] & ~ESW_U5_OPTR_RDP_MASK) | ESW_U5_RDP_L0));
    ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), ESW_U5_CR_OPTSTRT));
    ESW_TRY(flash_wait(t, 30000));   /* includes the mass erase */
    /* reload the option bytes: resets the chip, so the write's own status means nothing */
    (void)esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), ESW_U5_CR_OBL_LAUNCH);
    esw_sleep_ms(t->clk, 100);
    esw_memap_invalidate(&t->mem);
    ESW_TRY(esw_dap_connect(t->dap, NULL));
    ESW_TRY(esw_dap_power_up(t->dap, 100));
    ESW_TRY(u5_attach(t));
    return t->locked ? ESW_ERR_LOCKED : ESW_OK;
}

static uint32_t page_cmd(const esw_target_t *t, uint32_t addr)
{
    const uint32_t off = addr - ESW_U5_FLASH_BASE, bs = t->priv[P_BANK_SIZE];
    uint32_t bank2 = t->priv[P_DUAL] && off >= bs;
    if (t->priv[P_DUAL] && (t->priv[P_OPTR] & ESW_U5_OPTR_SWAP_BANK))
        bank2 ^= 1u;
    const uint32_t pnb = (off % bs) / ESW_U5_PAGE_SIZE;
    return ESW_U5_CR_PER | (pnb << ESW_U5_CR_PNB_POS) | (bank2 ? ESW_U5_CR_BKER : 0u);
}

static esw_err_t u5_erase(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, uint32_t len)
{
    if (r->kind == ESW_MEM_RAM)
        return t->cpu.ops->halt(&t->cpu);
    ESW_TRY(usable(t));
    ESW_TRY(flash_prepare(t));
    ESW_TRY(flash_unlock(t));
    esw_err_t e = ESW_OK;
    if (addr == r->start && len == r->size) {   /* whole flash: bank erase of both banks */
        const uint32_t mer = ESW_U5_CR_MER1 | (t->priv[P_DUAL] ? ESW_U5_CR_MER2 : 0u);
        e = esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), mer);
        if (e == ESW_OK)
            e = esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), mer | ESW_U5_CR_STRT);
        if (e == ESW_OK)
            e = flash_wait(t, 5000);
    } else {
        for (uint32_t a = addr - (addr - r->start) % r->sector_size; a < addr + len && e == ESW_OK;
             a += r->sector_size) {
            const uint32_t cmd = page_cmd(t, a);
            e = esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), cmd);
            if (e == ESW_OK)
                e = esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), cmd | ESW_U5_CR_STRT);
            if (e == ESW_OK)
                e = flash_wait(t, 200);
        }
    }
    esw_err_t e2 = flash_lock(t);
    icache_invalidate(t);
    return e != ESW_OK ? e : e2;
}

#define ESW_U5_CR_BWR (1u << 14)   /* burst: 8 quad-words (128 bytes, aligned) programmed in one operation */

/* Program n words (whole quad-words) at addr (16-byte aligned): 128-byte aligned stretches in burst mode, the edges
 * quad-word by quad-word. Each part goes out as one auto-increment block, paced by the flash interface's AHB
 * stall (WAIT on the DP), with one status check per part. */
static esw_err_t write_part(esw_target_t *t, uint32_t cr, uint32_t addr, const uint32_t *w, uint32_t n)
{
    ESW_TRY(esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), cr));
    ESW_TRY(esw_mem_write_block(&t->mem, addr, w, n));
    return flash_wait(t, 50);
}

static esw_err_t write_run(esw_target_t *t, uint32_t addr, const uint32_t *w, uint32_t n)
{
    uint32_t lead = ((128u - (addr & 127u)) & 127u) / 4u;
    if (lead > n)
        lead = n;
    if (lead)
        ESW_TRY(write_part(t, ESW_U5_CR_PG, addr, w, lead));
    const uint32_t nb = (n - lead) / 32u * 32u;
    if (nb)
        ESW_TRY(write_part(t, ESW_U5_CR_PG | ESW_U5_CR_BWR, addr + 4u * lead, w + lead, nb));
    const uint32_t tail = n - lead - nb;
    if (tail)
        ESW_TRY(write_part(t, ESW_U5_CR_PG, addr + 4u * (lead + nb), w + lead + nb, tail));
    return ESW_OK;
}

static uint32_t qw_erased(const uint32_t *w)
{
    return (w[0] & w[1] & w[2] & w[3]) == 0xFFFFFFFFu;
}

static esw_err_t u5_program(esw_target_t *t, const esw_mem_region_t *r, uint32_t addr, const uint32_t *data,
                            uint32_t nwords)
{
    if (r->kind == ESW_MEM_RAM) {
        ESW_TRY(t->cpu.ops->halt(&t->cpu));
        return esw_mem_write_block(&t->mem, addr, data, nwords);
    }
    ESW_TRY(usable(t));
    if (addr & 15u)
        return ESW_ERR_ARG;   /* quad-word aligned only */
    ESW_TRY(flash_prepare(t));
    ESW_TRY(flash_unlock(t));
    /* Runs of non-erased quad-words, each written by write_run(). */
    esw_err_t e = ESW_OK;
    const uint32_t full = nwords & ~3u;
    for (uint32_t i = 0; i < full && e == ESW_OK;) {
        if (qw_erased(&data[i])) {
            i += 4u;
            continue;
        }
        uint32_t j = i + 4u;
        while (j < full && !qw_erased(&data[j]))
            j += 4u;
        e = write_run(t, addr + 4u * i, &data[i], j - i);
        i = j;
    }
    if (e == ESW_OK && full < nwords) {   /* a short tail is padded with the erased value */
        uint32_t qw[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        for (uint32_t k = 0; full + k < nwords; k++)
            qw[k] = data[full + k];
        if (!qw_erased(qw))
            e = write_part(t, ESW_U5_CR_PG, addr + 4u * full, qw, 4);
    }
    esw_err_t e2 = esw_mem_write32(&t->mem, REG(ESW_U5_FLASH_NSCR), 0);
    esw_err_t e3 = flash_lock(t);
    icache_invalidate(t);
    return e != ESW_OK ? e : e2 != ESW_OK ? e2 : e3;
}

static esw_err_t u5_reset(esw_target_t *t, uint32_t flags)
{
    t->priv[P_CLEAN] = (flags & ESW_RESET_HALT) ? 1u : 0u;
    if (flags & ESW_RESET_HALT)
        return t->cpu.ops->sys_reset(&t->cpu, 1);
    (void)t->cpu.ops->debug_off(&t->cpu);
    return t->cpu.ops->sys_reset(&t->cpu, 0);
}

static esw_err_t u5_detach(esw_target_t *t)
{
    if (t->priv[P_FZR1_SET]) {
        (void)esw_mem_write32(&t->mem, ESW_U5_DBGMCU_APB1FZR1, t->priv[P_FZR1]);
        t->priv[P_FZR1_SET] = 0;
    }
    return t->cpu.ops->debug_off(&t->cpu);
}

const esw_target_driver_t esw_target_stm32u5 = {
    .name = "stm32u5",
    .probe = u5_probe,
    .attach = u5_attach,
    .unlock = u5_unlock,
    .erase = u5_erase,
    .program = u5_program,
    .reset = u5_reset,
    .detach = u5_detach,
};

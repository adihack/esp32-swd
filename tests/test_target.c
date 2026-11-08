/* SPDX-License-Identifier: MIT */
/* Target registry and the nRF52 module: detect, unlock (ERASEALL, no reset), NVMC program/verify, reset. */
#include "tinytest.h"
#include "rig.h"
#include "esw/target.h"
#include "esw/targets/nrf52.h"

/* the 64-byte LED-chase image flashed to the real DK (firmware/poc_swd/device/nrf_blinky.py) */
static const uint32_t blinky[16] = {
    0x20040000, 0x00000009, 0x215A4809, 0x48096001, 0x61814909, 0x22016081, 0x60C20352, 0x3B014B07,
    0x6082D1FD, 0x420A0052, 0x2201D1F7, 0xE7F40352, 0x40000558, 0x50000500, 0x0001E000, 0x00280000,
};

static esw_target_t tgt;

static esw_err_t attach(rig_t *r)
{
    ESW_TRY(esw_dap_connect(&r->dap, NULL));
    ESW_TRY(esw_dap_power_up(&r->dap, 10));
    return esw_target_detect(&tgt, &r->dap, &r->clk);
}

static void detect_unlocked(void)
{
    rig_t *r = rig_init(0);
    CHECK_OK(attach(r));
    CHECK(tgt.drv == &esw_target_nrf52);
    CHECK_EQ(tgt.locked, 0);
    CHECK(strcmp(tgt.part, "nRF52840-AAF0") == 0);
    CHECK_EQ(tgt.nregions, 3);
    CHECK(esw_target_region(&tgt, 0, 64) != NULL);
    CHECK(esw_target_region(&tgt, SIM_FLASH_SIZE - 4, 8) == NULL);   /* runs past the end */
}

static void locked_then_unlock_without_reset(void)
{
    rig_t *r = rig_init(1);
    CHECK_OK(attach(r));
    CHECK_EQ(tgt.locked, 1);
    CHECK_EQ(tgt.nregions, 0);
    CHECK_ERR(esw_target_program(&tgt, 0, blinky, 16), ESW_ERR_NOT_FOUND);
    CHECK_ERR(tgt.drv->unlock(&tgt, 0), ESW_ERR_LOCKED);              /* erase not allowed: refuse */
    uint32_t resets = r->sim.resets;
    CHECK_OK(tgt.drv->unlock(&tgt, ESW_UNLOCK_ALLOW_ERASE));
    CHECK_EQ(tgt.locked, 0);
    CHECK_EQ(r->sim.resets, resets);                                  /* no reset before UICR is HwDisabled */
    CHECK_EQ(r->sim.uicr[0x208 / 4], ESW_NRF_APPROTECT_HWDISABLED);
    CHECK(strcmp(tgt.part, "nRF52840-AAF0") == 0);
}

static void program_verify_reset(void)
{
    rig_t *r = rig_init(1);
    CHECK_OK(attach(r));
    CHECK_OK(tgt.drv->unlock(&tgt, ESW_UNLOCK_ALLOW_ERASE));
    CHECK_OK(esw_target_program(&tgt, 0, blinky, 16));
    CHECK(memcmp(r->sim.flash, blinky, sizeof(blinky)) == 0);
    CHECK_EQ(r->sim.nvmc_busy_reads, 0);                              /* never wrote while NVMC was busy */
    CHECK_OK(esw_target_verify(&tgt, 0, blinky, 16));
    uint32_t bad[16];
    memcpy(bad, blinky, sizeof(bad));
    bad[7] ^= 1u;
    CHECK_ERR(esw_target_verify(&tgt, 0, bad, 16), ESW_ERR_VERIFY);
    /* page erase brings it back to 0xFF */
    CHECK_OK(esw_target_erase(&tgt, 0, 4096));
    CHECK_EQ(r->sim.flash[0], 0xFFFFFFFFu);
    CHECK_OK(esw_target_program(&tgt, 0, blinky, 16));
    /* reset: image + UICR HwDisabled -> the chip stays open after reset (as on the real DK) */
    CHECK_OK(tgt.drv->reset(&tgt, 0));
    uint32_t open = 0;
    CHECK_OK(esw_nrf52_approtect_open(&tgt, &open));
    CHECK_EQ(open, 1);
    CHECK_EQ(r->sim.contention, 0);
}

static void bulk_program_paced_by_stall(void)
{
    static uint32_t img[3000];
    rig_t *r = rig_init(0);
    for (uint32_t i = 0; i < 3000; i++) img[i] = 0x9E3779B9u * (i + 1u);
    CHECK_OK(attach(r));
    uint32_t before = r->dap.stats.xfers;
    CHECK_OK(esw_target_program(&tgt, 0x1000, img, 3000));          /* crosses several 1 KB TAR boundaries */
    CHECK(memcmp(&r->sim.flash[0x1000 / 4], img, sizeof(img)) == 0);
    CHECK_EQ(r->sim.nvmc_busy_reads, 0);                             /* no write slipped past a busy NVMC */
    CHECK(r->sim.stalls > 0);                                        /* pacing came from the bus stall */
    CHECK(r->dap.stats.xfers - before < 3000u + r->sim.stalls + 64u);  /* ~1 packet per word */
    CHECK_OK(esw_target_verify(&tgt, 0x1000, img, 3000));
}

static void whole_flash_erase_keeps_uicr(void)
{
    rig_t *r = rig_init(0);
    const uint32_t hw = ESW_NRF_APPROTECT_HWDISABLED, ps = 0x12;
    CHECK_OK(attach(r));
    CHECK_OK(esw_target_program(&tgt, 0x8000, blinky, 16));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_APPROTECT, &hw, 1));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_PSELRESET0, &ps, 1));
    CHECK_OK(esw_target_erase(&tgt, 0, SIM_FLASH_SIZE));           /* whole region -> one ERASEALL */
    CHECK_EQ(r->sim.flash[0x8000 / 4], 0xFFFFFFFFu);
    CHECK_EQ(r->sim.uicr[0x208 / 4], ESW_NRF_APPROTECT_HWDISABLED);  /* UICR as before: like a page-by-page erase */
    CHECK_EQ(r->sim.uicr[0x200 / 4], 0x12u);
    CHECK_EQ(r->sim.uicr[0x080 / 4], 0xFFFFFFFFu);
}

/* CRC loader stand-ins at the blob's entry points (the real Thumb blob is verified in unicorn by
 * loaders/crc32/build.py); they also record that the probe downloaded the blob first. */
static uint32_t g_crc_calls, g_crc_blob_ok;
static uint32_t ref_crc(swd_sim_t *s, uint32_t addr, uint32_t len, uint32_t crc)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (sim_ram_rd(s, (addr + i) & ~3u) >> (8u * ((addr + i) & 3u))) & 0xFFu;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static uint32_t h_crc_init(swd_sim_t *s, uint32_t *regs, int first)
{
    (void)first;
    g_crc_blob_ok = sim_ram_rd(s, 0x20000000u) == 0xE7FDBE00u && regs[0] == 0x20000400u && regs[13] == 0x20001000u;
    return 0;
}
static uint32_t h_crc_update(swd_sim_t *s, uint32_t *regs, int first)
{
    (void)first;
    g_crc_calls++;
    return ref_crc(s, regs[0], regs[1], regs[3]);
}

static void crc_verify_on_target_and_fallback(void)
{
    static uint32_t img[4096];
    rig_t *r = rig_init(0);
    const uint32_t hw = ESW_NRF_APPROTECT_HWDISABLED;
    for (uint32_t i = 0; i < 4096; i++) img[i] = 0x6D2B79F5u * (i + 7u);
    CHECK_OK(attach(r));
    CHECK_EQ(tgt.scratch_ram, 0x20000000u);
    CHECK_OK(esw_target_program(&tgt, 0, img, 4096));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_APPROTECT, &hw, 1));
    swd_sim_add_hook(&r->sim, 0x20000004u, SIM_HOOK_CALL, h_crc_init);
    swd_sim_add_hook(&r->sim, 0x20000038u, SIM_HOOK_CALL, h_crc_update);
    uint32_t want = ref_crc(&r->sim, 0, 16384, 0), crc = 0;
    g_crc_calls = 0;
    CHECK_OK(esw_target_crc32(&tgt, 0, 16384, ESW_CRC_RESET_HALT, &crc));
    CHECK_EQ(crc, want);
    CHECK_EQ(g_crc_blob_ok, 1);
    CHECK_EQ(g_crc_calls, 1);                                    /* computed on the target */
    CHECK_OK(esw_target_crc32(&tgt, 4, 16380 - 2, 0, &crc));      /* odd length, chained from the start */
    CHECK_EQ(crc, ref_crc(&r->sim, 4, 16378, 0));
    /* a RAM range over the loader's scratch area is read back (and left intact), never run through the loader */
    for (uint32_t i = 0; i < 1024; i++) r->sim.ram[i] = 0x13579BDFu ^ i;
    uint32_t ram_want = ref_crc(&r->sim, 0x20000000u, 8192, 0);
    g_crc_calls = 0;
    CHECK_OK(esw_target_crc32(&tgt, 0x20000000u, 8192, 0, &crc));
    CHECK_EQ(crc, ram_want);
    CHECK_EQ(g_crc_calls, 0);
    CHECK_EQ(r->sim.ram[0], 0x13579BDFu);
    tgt.scratch_ram = 0;                                          /* no scratch RAM: read-back on the probe */
    g_crc_calls = 0;
    CHECK_OK(esw_target_crc32(&tgt, 0, 16384, 0, &crc));
    CHECK_EQ(crc, want);
    CHECK_EQ(g_crc_calls, 0);
    CHECK_EQ(r->sim.contention, 0);
}

static void reset_with_halt(void)
{
    rig_t *r = rig_init(0);
    const uint32_t hw = ESW_NRF_APPROTECT_HWDISABLED;
    CHECK_OK(attach(r));
    CHECK_OK(esw_target_program(&tgt, 0, blinky, 16));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_APPROTECT, &hw, 1));   /* UICR region, through the NVMC */
    CHECK_OK(tgt.drv->reset(&tgt, ESW_RESET_HALT));
    CHECK_EQ(r->sim.halted, 1);
    CHECK_EQ(r->sim.demcr & 1u, 0);   /* vector catch cleared again */
    CHECK_EQ(r->sim.regs[15], 0x00000008u);   /* halted at the image's reset handler */
}

static void reset_halt_right_after_attach(void)
{
    /* review finding: vector catch needs C_DEBUGEN; a reset-halt with no earlier halt used to time out */
    rig_t *r = rig_init(0);
    const uint32_t hw = ESW_NRF_APPROTECT_HWDISABLED;
    CHECK_OK(attach(r));
    CHECK_OK(esw_target_program(&tgt, 0, blinky, 16));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_APPROTECT, &hw, 1));
    CHECK_OK(tgt.drv->reset(&tgt, 0));                  /* debug off: C_DEBUGEN = 0 now */
    CHECK_EQ(r->sim.debugen, 0);
    CHECK_OK(tgt.drv->reset(&tgt, ESW_RESET_HALT));
    CHECK_EQ(r->sim.halted, 1);
    CHECK_EQ(r->sim.regs[15], 0x00000008u);
    CHECK_EQ(r->sim.demcr & 1u, 0);
}

static uint32_t g_hook_runs;
static uint32_t count_hook(swd_sim_t *s, uint32_t *regs, int first) { (void)s; (void)regs; (void)first; g_hook_runs++; return 0; }

static void reset_halt_never_resumes_a_halted_core(void)
{
    /* review 2: writing C_DEBUGEN alone resumed an already-halted core, which then ran erased/old code */
    rig_t *r = rig_init(0);
    const uint32_t hw = ESW_NRF_APPROTECT_HWDISABLED;
    CHECK_OK(attach(r));
    CHECK_OK(esw_target_program(&tgt, 0, blinky, 16));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_APPROTECT, &hw, 1));    /* UICR erased would lock at reset */
    g_hook_runs = 0;
    swd_sim_add_hook(&r->sim, 0x20000100u, SIM_HOOK_CALL, count_hook);
    CHECK_OK(tgt.cpu.ops->halt(&tgt.cpu));
    CHECK_OK(tgt.cpu.ops->reg_write(&tgt.cpu, ESW_REG_PC, 0x20000100u));   /* "stale code" at the halt PC */
    CHECK_OK(tgt.cpu.ops->sys_reset(&tgt.cpu, 1));
    CHECK_EQ(g_hook_runs, 0);
    CHECK_EQ(r->sim.halted, 1);
}

static void reset_halt_survives_lost_reset_flag(void)
{
    /* review 2: S_RESET_ST is clear-on-read; if a discarded read consumed it, DFSR.VCATCH must still confirm */
    rig_t *r = rig_init(0);
    const uint32_t hw = ESW_NRF_APPROTECT_HWDISABLED;
    CHECK_OK(attach(r));
    CHECK_OK(esw_target_program(&tgt, 0, blinky, 16));
    CHECK_OK(esw_target_program(&tgt, ESW_NRF_UICR_APPROTECT, &hw, 1));
    r->sim.lose_reset_st = 1;
    CHECK_OK(tgt.cpu.ops->sys_reset(&tgt.cpu, 1));
    CHECK_EQ(r->sim.halted, 1);
    CHECK_EQ(r->sim.demcr & 1u, 0);
}

static void reset_on_locked_chip_still_resets(void)
{
    /* review finding: the DHCSR write faults while locked and used to abort before the CTRL-AP reset */
    rig_t *r = rig_init(1);
    CHECK_OK(attach(r));
    CHECK_EQ(tgt.locked, 1);
    uint32_t resets = r->sim.resets;
    CHECK_ERR(tgt.drv->reset(&tgt, 0), ESW_ERR_LOCKED);   /* still locked afterwards (blank chip) */
    CHECK_EQ(r->sim.resets, resets + 1);                    /* but the reset was issued */
}

static void erased_uicr_locks_on_reset(void)
{
    rig_t *r = rig_init(0);
    CHECK_OK(attach(r));
    CHECK_ERR(tgt.drv->reset(&tgt, 0), ESW_ERR_LOCKED);
    CHECK_EQ(tgt.locked, 1);
    (void)r;
}

int main(void)
{
    RUN(detect_unlocked);
    RUN(locked_then_unlock_without_reset);
    RUN(program_verify_reset);
    RUN(bulk_program_paced_by_stall);
    RUN(whole_flash_erase_keeps_uicr);
    RUN(crc_verify_on_target_and_fallback);
    RUN(reset_with_halt);
    RUN(erased_uicr_locks_on_reset);
    RUN(reset_halt_right_after_attach);
    RUN(reset_on_locked_chip_still_resets);
    RUN(reset_halt_never_resumes_a_halted_core);
    RUN(reset_halt_survives_lost_reset_flag);
    return TT_RESULT();
}

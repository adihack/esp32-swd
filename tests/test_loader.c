/* SPDX-License-Identifier: MIT */
/* Generic loader runtime: call ABI (FLM-style) and streaming ABI, on an external-flash region that the
 * debugger cannot reach directly. Loader code is emulated by simulator hooks at the descriptor's entry points. */
#include "tinytest.h"
#include "rig.h"
#include "esw/target.h"
#include "esw/loader.h"

#define LDR_BASE   0x20001000u
#define TRAP       (LDR_BASE + 0x00u)
#define FN_INIT    (LDR_BASE + 0x11u)
#define FN_ERASE   (LDR_BASE + 0x21u)
#define FN_PROG    (LDR_BASE + 0x31u)
#define FN_STREAM  (LDR_BASE + 0x41u)
#define FN_HANG    (LDR_BASE + 0x51u)
#define PAGE_BUF   0x20002000u
#define MAILBOX    0x20003000u
#define RING       0x20004000u
#define NBUF       2u
#define BUFSIZE    1024u

static uint32_t blob[32];
static esw_target_t tgt;
static struct { uint32_t init, erase, prog, prog_fail, max_filled, crc_cmds; } seen;

/* ---- emulated loader code -------------------------------------------------------------------------------- */
static uint32_t g_demcr_in_call;
static uint32_t h_init(swd_sim_t *s, uint32_t *regs, int first)
{
    (void)regs;
    (void)first;
    seen.init++;
    g_demcr_in_call = s->demcr;
    return 0;
}

static uint32_t ext_erase(swd_sim_t *s, uint32_t addr, uint32_t len)
{
    if (addr < SIM_EXT_BASE || addr + len > SIM_EXT_BASE + SIM_EXT_SIZE) return 1;
    for (uint32_t a = addr & ~(SIM_EXT_SECTOR - 1u); a < addr + len; a += SIM_EXT_SECTOR)
        memset(&s->ext[(a - SIM_EXT_BASE) / 4], 0xFF, SIM_EXT_SECTOR);
    return 0;
}

static uint32_t ext_program(swd_sim_t *s, uint32_t addr, uint32_t len, uint32_t buf)
{
    if (addr < SIM_EXT_BASE || addr + len > SIM_EXT_BASE + SIM_EXT_SIZE || (len & 3u)) return 2;
    for (uint32_t i = 0; i < len / 4; i++)
        sim_ram_wr(s, addr + 4 * i, sim_ram_rd(s, buf + 4 * i));
    return 0;
}

static uint32_t h_erase(swd_sim_t *s, uint32_t *regs, int first)
{
    (void)first;
    seen.erase++;
    return ext_erase(s, regs[0], SIM_EXT_SECTOR);
}

static uint32_t h_prog(swd_sim_t *s, uint32_t *regs, int first)
{
    (void)first;
    seen.prog++;
    if (seen.prog_fail && seen.prog == seen.prog_fail) return 7;
    return ext_program(s, regs[0], regs[1], regs[2]);
}

static uint32_t crc32_ext(swd_sim_t *s, uint32_t addr, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t w = sim_ram_rd(s, (addr + i) & ~3u);
        crc ^= (w >> (8u * ((addr + i) & 3u))) & 0xFFu;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

#define MBW(off) (MAILBOX + (off))
#define SLOTW(i, w) (MAILBOX + ESW_LDR_MB_SLOTS + ((i) * ESW_LDR_SLOT_WORDS + (w)) * 4u)

/* Resident streaming loader: one step per probe AP access. Programming a buffer takes PROG_STEPS steps, so the
 * probe has to fill the next buffer while the loader is still busy with the previous one. */
#define PROG_STEPS 400u
static uint32_t h_stream(swd_sim_t *s, uint32_t *regs, int first)
{
    static uint32_t next, left;
    static int busy;
    (void)regs;
    if (first) {
        sim_ram_wr(s, MBW(ESW_LDR_MB_VERSION), ESW_LDR_ABI_VERSION);
        sim_ram_wr(s, MBW(ESW_LDR_MB_CAPS), ESW_LDR_CAP_ERASE | ESW_LDR_CAP_PROGRAM | ESW_LDR_CAP_CRC32);
        sim_ram_wr(s, MBW(ESW_LDR_MB_NBUF), NBUF);
        sim_ram_wr(s, MBW(ESW_LDR_MB_BUFSIZE), BUFSIZE);
        sim_ram_wr(s, MBW(ESW_LDR_MB_BUFBASE), RING);
        sim_ram_wr(s, MBW(ESW_LDR_MB_CMD_SEQ), 0);
        sim_ram_wr(s, MBW(ESW_LDR_MB_DONE_SEQ), 0);
        for (uint32_t i = 0; i < NBUF; i++) sim_ram_wr(s, SLOTW(i, 0), ESW_LDR_SLOT_FREE);
        sim_ram_wr(s, MBW(ESW_LDR_MB_MAGIC), ESW_LDR_MAGIC);
        next = 0;
        left = 0;
        busy = -1;
        return 0;
    }
    uint32_t pending = 0;
    for (uint32_t i = 0; i < NBUF; i++) {
        uint32_t st = sim_ram_rd(s, SLOTW(i, 0));
        pending += st == ESW_LDR_SLOT_FILLED || st == ESW_LDR_SLOT_BUSY;
    }
    if (pending > seen.max_filled) seen.max_filled = pending;
    uint32_t seq = sim_ram_rd(s, MBW(ESW_LDR_MB_CMD_SEQ));
    if (busy < 0 && seq != sim_ram_rd(s, MBW(ESW_LDR_MB_DONE_SEQ))) {
        uint32_t cmd = sim_ram_rd(s, MBW(ESW_LDR_MB_CMD)), a = sim_ram_rd(s, MBW(ESW_LDR_MB_ADDR));
        uint32_t len = sim_ram_rd(s, MBW(ESW_LDR_MB_LEN)), st = 0, res = 0;
        if (cmd == ESW_LDR_CMD_ERASE) st = ext_erase(s, a, len);
        else if (cmd == ESW_LDR_CMD_CRC32) { res = crc32_ext(s, a, len); seen.crc_cmds++; }
        else if (cmd != ESW_LDR_CMD_EXIT) st = 0x55;
        sim_ram_wr(s, MBW(ESW_LDR_MB_STATUS), st);
        sim_ram_wr(s, MBW(ESW_LDR_MB_RESULT), res);
        sim_ram_wr(s, MBW(ESW_LDR_MB_DONE_SEQ), seq);
        return cmd == ESW_LDR_CMD_EXIT;
    }
    if (busy < 0 && sim_ram_rd(s, SLOTW(next, 0)) == ESW_LDR_SLOT_FILLED) {
        busy = (int)next;
        left = PROG_STEPS;
        sim_ram_wr(s, SLOTW(next, 0), ESW_LDR_SLOT_BUSY);
    }
    if (busy >= 0 && left-- == 0) {
        uint32_t i = (uint32_t)busy;
        uint32_t st = ext_program(s, sim_ram_rd(s, SLOTW(i, 1)), sim_ram_rd(s, SLOTW(i, 2)), RING + i * BUFSIZE);
        seen.prog++;
        sim_ram_wr(s, SLOTW(i, 3), st);
        sim_ram_wr(s, SLOTW(i, 0), ESW_LDR_SLOT_DONE);
        busy = -1;
        next = (next + 1) % NBUF;
    }
    return 0;
}

static uint32_t h_hang(swd_sim_t *s, uint32_t *regs, int first) { (void)s; (void)regs; (void)first; return 0; }

/* ---- fixture -------------------------------------------------------------------------------------------- */
static esw_loader_desc_t desc;

static rig_t *setup(uint32_t stream)
{
    rig_t *r = rig_init(0);
    memset(&seen, 0, sizeof(seen));
    for (uint32_t i = 0; i < 32; i++) blob[i] = 0xBE00BE00u ^ (i << 16);   /* word 0: BKPT #0 | BKPT #0 */
    blob[0] = 0xBE00BE00u;
    memset(&desc, 0, sizeof(desc));
    desc.load_addr = LDR_BASE;
    desc.blob = blob;
    desc.blob_words = 32;
    desc.stack_top = 0x20008000u;
    desc.trap_addr = TRAP;
    desc.fn_init = FN_INIT;
    desc.fn_erase_sector = FN_ERASE;
    desc.fn_program_page = FN_PROG;
    desc.page_buf_addr = PAGE_BUF;
    desc.page_size = 256;
    desc.fn_stream = stream ? FN_STREAM : 0;
    desc.mailbox_addr = stream ? MAILBOX : 0;
    desc.timeout_call_ms = 2000;
    desc.timeout_erase_ms = 500;
    swd_sim_add_hook(&r->sim, FN_INIT, SIM_HOOK_CALL, h_init);
    swd_sim_add_hook(&r->sim, FN_ERASE, SIM_HOOK_CALL, h_erase);
    swd_sim_add_hook(&r->sim, FN_PROG, SIM_HOOK_CALL, h_prog);
    swd_sim_add_hook(&r->sim, FN_STREAM, SIM_HOOK_RESIDENT, h_stream);
    swd_sim_add_hook(&r->sim, FN_HANG, SIM_HOOK_RESIDENT, h_hang);
    if (esw_dap_connect(&r->dap, NULL) || esw_dap_power_up(&r->dap, 10) ||
        esw_target_detect(&tgt, &r->dap, &r->clk))
        return NULL;
    esw_mem_region_t ext = { "ext", ESW_MEM_EXT, SIM_EXT_BASE, SIM_EXT_SIZE, SIM_EXT_SECTOR, &desc, stream };
    if (esw_target_add_region(&tgt, &ext))
        return NULL;
    return r;
}

static uint32_t image[3000];

static void fill_image(void)
{
    for (uint32_t i = 0; i < 3000; i++) image[i] = 0xA5000000u ^ (i * 40503u);
}

/* ---- tests ---------------------------------------------------------------------------------------------- */
static void ext_flash_not_reachable_directly(void)
{
    rig_t *r = setup(0);
    CHECK(r != NULL);
    uint32_t v = 0;
    CHECK_ERR(esw_mem_read32(&tgt.mem, SIM_EXT_BASE, &v), ESW_ERR_FAULT);
}

/* program() returns before its last buffers are programmed; a failure there comes back from the next sync */
static void stream_error_reported_at_sync(void)
{
    rig_t *r = setup(1);
    CHECK(r != NULL);
    fill_image();
    CHECK_OK(esw_target_erase(&tgt, SIM_EXT_BASE, 4096));
    CHECK_OK(esw_loader_stream_program(&tgt.loader, SIM_EXT_BASE + SIM_EXT_SIZE - 16u, image, 64));   /* runs past */
    CHECK_ERR(esw_target_sync(&tgt), ESW_ERR_LOADER);
    CHECK_EQ(tgt.loader.last_status, 2);
    CHECK_OK(esw_target_sync(&tgt));   /* reported once; the ring is free again */
    CHECK_OK(esw_target_program(&tgt, SIM_EXT_BASE, image, 1000));
    CHECK_OK(esw_target_verify(&tgt, SIM_EXT_BASE, image, 1000));
    CHECK_OK(esw_target_finish(&tgt));
}

static void call_abi_erase_program(void)
{
    rig_t *r = setup(0);
    CHECK(r != NULL);
    fill_image();
    memset(r->sim.ext, 0, 8192);
    CHECK_OK(esw_target_erase(&tgt, SIM_EXT_BASE, 8192));
    CHECK_EQ(seen.init, 1);                     /* loader downloaded and Init called once */
    CHECK_EQ(g_demcr_in_call & 0x401u, 0x401u);  /* reset + HardFault vector catch armed while loader code ran */
    CHECK_EQ(r->sim.demcr & 0x401u, 0);          /* and restored afterwards */
    CHECK_EQ(seen.erase, 2);
    CHECK_EQ(r->sim.ext[0], 0xFFFFFFFFu);
    CHECK(memcmp(&r->sim.ram[(LDR_BASE - SIM_RAM_BASE) / 4], blob, sizeof(blob)) == 0);
    CHECK_OK(esw_target_program(&tgt, SIM_EXT_BASE, image, 1000));
    CHECK_EQ(seen.prog, 16);                    /* 4000 bytes / 256-byte pages */
    CHECK(memcmp(r->sim.ext, image, 4000) == 0);
    CHECK_EQ(r->sim.regs[15], TRAP);            /* every call returned into the trap */
    CHECK_EQ(seen.init, 1);
    CHECK_ERR(esw_target_verify(&tgt, SIM_EXT_BASE, image, 1000), ESW_ERR_UNSUPPORTED);
    CHECK_OK(esw_target_finish(&tgt));
    CHECK_EQ(r->sim.contention, 0);
}

static void call_abi_error_status(void)
{
    rig_t *r = setup(0);
    CHECK(r != NULL);
    fill_image();
    seen.prog_fail = 3;
    CHECK_ERR(esw_target_program(&tgt, SIM_EXT_BASE, image, 1000), ESW_ERR_LOADER);
    CHECK_EQ(tgt.loader.last_status, 7);
}

static void call_timeout_halts_core(void)
{
    rig_t *r = setup(0);
    CHECK(r != NULL);
    esw_loader_t l;
    CHECK_OK(esw_loader_load(&l, &tgt.cpu, &r->clk, &desc));
    uint32_t args[4] = { 0, 0, 0, 0 };
    CHECK_ERR(esw_loader_call(&l, FN_HANG, args, 50, NULL), ESW_ERR_TIMEOUT);
    CHECK_EQ(r->sim.halted, 1);
}

static void stream_abi_program_and_crc_verify(void)
{
    rig_t *r = setup(1);
    CHECK(r != NULL);
    fill_image();
    CHECK_OK(esw_target_erase(&tgt, SIM_EXT_BASE, 12000));
    CHECK_EQ(tgt.loader.streaming, 1);
    CHECK_EQ(tgt.loader.nbuf, NBUF);
    CHECK_OK(esw_target_program(&tgt, SIM_EXT_BASE, image, 3000));   /* 12000 bytes: 12 buffers through a ring of 2 */
    CHECK_OK(esw_target_sync(&tgt));   /* program returns once handed over: the last buffers land here */
    CHECK(memcmp(r->sim.ext, image, 12000) == 0);
    CHECK_EQ(seen.prog, 12);
    CHECK_EQ(seen.max_filled, 2);               /* next buffer handed over while the previous was still programming */
    CHECK_EQ(r->sim.halted, 0);                 /* no halt per page: the loader kept running */
    CHECK_OK(esw_target_verify(&tgt, SIM_EXT_BASE, image, 3000));
    CHECK_EQ(seen.crc_cmds, 1);                 /* verified by CRC on the target, not by read-back */
    r->sim.ext[100] ^= 0x10u;
    CHECK_ERR(esw_target_verify(&tgt, SIM_EXT_BASE, image, 3000), ESW_ERR_VERIFY);
    CHECK_OK(esw_target_finish(&tgt));
    CHECK_EQ(r->sim.halted, 1);                 /* EXIT returned into the trap */
    CHECK_EQ(r->sim.regs[15], TRAP);
    CHECK_EQ(r->sim.contention, 0);
}

int main(void)
{
    RUN(ext_flash_not_reachable_directly);
    RUN(call_abi_erase_program);
    RUN(call_abi_error_status);
    RUN(call_timeout_halts_core);
    RUN(stream_abi_program_and_crc_verify);
    RUN(stream_error_reported_at_sync);
    return TT_RESULT();
}

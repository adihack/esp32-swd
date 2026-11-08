/* SPDX-License-Identifier: MIT */
#include "swd_sim.h"
#include "esw/swd_phy.h"
#include <string.h>

enum { S_IDLE, S_REQ, S_ACK, S_RDATA, S_RPAR, S_RELEASE, S_WTRN, S_WDATA, S_WPAR };

#define CS_STICKYORUN (1u << 1)
#define CS_STICKYCMP  (1u << 4)
#define CS_STICKYERR  (1u << 5)
#define CS_WDATAERR   (1u << 7)
#define CS_STICKY     (CS_STICKYORUN | CS_STICKYCMP | CS_STICKYERR | CS_WDATAERR)
#define CS_CDBGPWRUPREQ (1u << 28)
#define CS_CSYSPWRUPREQ (1u << 30)

#define NVMC_BUSY_READS 2u

/* ---- memory model ------------------------------------------------------------------------------------- */

uint32_t sim_ram_rd(swd_sim_t *s, uint32_t a)
{
    if (a < SIM_FLASH_SIZE)
        return s->flash[a / 4];
    if (a >= SIM_RAM_BASE && a < SIM_RAM_BASE + SIM_RAM_SIZE)
        return s->ram[(a - SIM_RAM_BASE) / 4];
    if (a >= SIM_EXT_BASE && a < SIM_EXT_BASE + SIM_EXT_SIZE)
        return s->ext[(a - SIM_EXT_BASE) / 4];
    return 0;
}

void sim_ram_wr(swd_sim_t *s, uint32_t a, uint32_t v)
{
    if (a >= SIM_RAM_BASE && a < SIM_RAM_BASE + SIM_RAM_SIZE)
        s->ram[(a - SIM_RAM_BASE) / 4] = v;
    else if (a >= SIM_EXT_BASE && a < SIM_EXT_BASE + SIM_EXT_SIZE)
        s->ext[(a - SIM_EXT_BASE) / 4] &= v;   /* NOR semantics: program clears bits */
}

static uint32_t ficr(uint32_t a)
{
    switch (a) {
    case 0x10000010u: return 4096;            /* CODEPAGESIZE */
    case 0x10000014u: return SIM_FLASH_SIZE / 4096;
    case 0x10000060u: return 0x22CACD6Bu;
    case 0x10000064u: return 0x9F54AEC6u;
    case 0x10000100u: return 0x00052840u;
    case 0x10000104u: return 0x41414630u;     /* "AAF0" */
    case 0x10000108u: return 0x00002004u;
    case 0x1000010Cu: return SIM_RAM_SIZE / 1024;
    case 0x10000110u: return SIM_FLASH_SIZE / 1024;
    default: return 0xFFFFFFFFu;
    }
}

static void do_reset(swd_sim_t *s)
{
    s->resets++;
    s->halted = ((s->demcr & 1u) && s->debugen) ? 1u : 0u;   /* VC_CORERESET is ignored without C_DEBUGEN */
    if (s->halted)
        s->dfsr |= 0x8u;   /* VCATCH */
    s->reset_st = s->lose_reset_st ? 0u : 1u;
    s->resident = -1;
    s->regs[13] = s->flash[0];
    s->regs[15] = s->flash[1] & ~1u;
    /* Measured on an nRF52840 AAF0 (build code F), 2026-11-06, debug session kept across the reset: with
     * UICR.APPROTECT = HwDisabled the chip stays open after reset-halt and normal reset whatever the flash holds
     * (valid image, blank, random). With UICR erased the hardware APPROTECT engages at reset. */
    s->approtect_open = (s->uicr[0x208 / 4] == 0xFFFFFF5Au) ? 1u : 0u;
}

static void nvmc_store(swd_sim_t *s, uint32_t *word, uint32_t v, int *ok)
{
    if (s->nvmc_config != 1u) {
        *ok = 0;   /* write without WEN: bus error */
        return;
    }
    if (s->nvmc_busy)
        s->nvmc_busy_reads++;   /* counted: a write while the previous one is still running */
    *word &= v;
    s->nvmc_busy = NVMC_BUSY_READS;
}

static void run_hook_on_resume(swd_sim_t *s);

static int mem_read(swd_sim_t *s, uint32_t a, uint32_t *v)
{
    if (a < SIM_FLASH_SIZE) { *v = s->flash[a / 4]; return 1; }
    if (a >= 0x10000000u && a < 0x10001000u) { *v = ficr(a); return 1; }
    if (a >= SIM_UICR_BASE && a < SIM_UICR_BASE + 0x1000u) { *v = s->uicr[(a - SIM_UICR_BASE) / 4]; return 1; }
    if (a >= SIM_RAM_BASE && a < SIM_RAM_BASE + SIM_RAM_SIZE) { *v = s->ram[(a - SIM_RAM_BASE) / 4]; return 1; }
    switch (a) {
    case 0x4001E400u:   /* NVMC READY */
        if (s->nvmc_busy) { s->nvmc_busy--; *v = 0; } else *v = 1;
        return 1;
    case 0x4001E408u: *v = 1; return 1;
    case 0x4001E504u: *v = s->nvmc_config; return 1;
    case 0x50000504u: *v = s->gpio_out; return 1;
    case 0xE000ED00u: *v = 0x410FC241u; return 1;
    case 0xE000ED30u: *v = s->dfsr; return 1;
    case 0xE000EDF0u:
        *v = (s->halted ? (1u << 17) | 2u : 0u) | (1u << 16) | (s->debugen ? 1u : 0u) | (s->reset_st << 25);
        s->reset_st = 0;   /* S_RESET_ST clears on read */
        return 1;
    case 0xE000EDF8u: *v = s->dcrdr; return 1;
    case 0xE000EDFCu: *v = s->demcr; return 1;
    default: return 0;
    }
}

static int mem_write(swd_sim_t *s, uint32_t a, uint32_t v)
{
    int ok = 1;
    if (a < SIM_FLASH_SIZE) { nvmc_store(s, &s->flash[a / 4], v, &ok); return ok; }
    if (a >= SIM_UICR_BASE && a < SIM_UICR_BASE + 0x1000u) {
        nvmc_store(s, &s->uicr[(a - SIM_UICR_BASE) / 4], v, &ok);
        return ok;
    }
    if (a >= SIM_RAM_BASE && a < SIM_RAM_BASE + SIM_RAM_SIZE) { s->ram[(a - SIM_RAM_BASE) / 4] = v; return 1; }
    switch (a) {
    case 0x4001E504u: s->nvmc_config = v & 3u; return 1;
    case 0x4001E508u:   /* ERASEPAGE */
        if (s->nvmc_config != 2u || v >= SIM_FLASH_SIZE) return 0;
        memset(&s->flash[(v & ~0xFFFu) / 4], 0xFF, 4096);
        s->nvmc_busy = 5;
        return 1;
    case 0x4001E50Cu:   /* ERASEALL: flash + UICR */
        if (s->nvmc_config != 2u || !(v & 1u)) return 0;
        memset(s->flash, 0xFF, sizeof(s->flash));
        memset(s->uicr, 0xFF, sizeof(s->uicr));
        s->nvmc_busy = 8;
        return 1;
    case 0x4001E514u:   /* ERASEUICR */
        if (s->nvmc_config != 2u) return 0;
        memset(s->uicr, 0xFF, sizeof(s->uicr));
        s->nvmc_busy = 5;
        return 1;
    case 0x50000504u: s->gpio_out = v; return 1;
    case 0xE000ED0Cu:
        if (v == 0x05FA0004u) do_reset(s);
        return 1;
    case 0xE000ED30u: s->dfsr &= ~v; return 1;
    case 0xE000EDF0u:
        if ((v & 0xFFFF0000u) != 0xA05F0000u) return 1;
        s->debugen = v & 1u;
        if ((v & 1u) && (v & 2u)) {
            if (!s->halted)
                s->dfsr |= 0x1u;   /* HALTED */
            s->halted = 1;
            s->resident = -1;
        } else if (s->halted && !(v & 2u)) {
            s->halted = 0;
            run_hook_on_resume(s);
        }
        return 1;
    case 0xE000EDF4u: {
        uint32_t sel = v & 0x7Fu;
        if (sel < 24) {
            if (v & (1u << 16)) s->regs[sel] = s->dcrdr;
            else s->dcrdr = s->regs[sel];
        }
        return 1;
    }
    case 0xE000EDF8u: s->dcrdr = v; return 1;
    case 0xE000EDFCu: s->demcr = v; return 1;
    default: return 0;
    }
}

/* ---- native-code hooks --------------------------------------------------------------------------------- */

void swd_sim_add_hook(swd_sim_t *s, uint32_t addr, sim_hook_kind_t kind, sim_hook_fn fn)
{
    s->hooks[s->nhooks].addr = addr & ~1u;
    s->hooks[s->nhooks].kind = kind;
    s->hooks[s->nhooks].fn = fn;
    s->nhooks++;
}

static void return_to_lr(swd_sim_t *s)
{
    s->regs[15] = s->regs[14] & ~1u;   /* LR points at the BKPT in the blob */
    s->halted = 1;
    s->dfsr |= 0x2u;                   /* BKPT */
    s->resident = -1;
}

static void run_hook_on_resume(swd_sim_t *s)
{
    for (uint32_t i = 0; i < s->nhooks; i++) {
        if (s->hooks[i].addr != (s->regs[15] & ~1u))
            continue;
        if (s->hooks[i].kind == SIM_HOOK_CALL) {
            s->regs[0] = s->hooks[i].fn(s, s->regs, 1);
            return_to_lr(s);
        } else {
            s->resident = (int)i;
            if (s->hooks[i].fn(s, s->regs, 1))
                return_to_lr(s);
        }
        return;
    }
    /* no hook: the core runs real firmware we do not model; it stays running */
}

static void step_resident(swd_sim_t *s)
{
    if (s->resident >= 0 && !s->halted) {
        if (s->hooks[s->resident].fn(s, s->regs, 0))
            return_to_lr(s);
    }
}

/* ---- DP / AP ---------------------------------------------------------------------------------------------- */

static uint32_t dp_read(swd_sim_t *s, uint32_t a)
{
    switch (a) {
    case 0x0: s->lockout = 0; return 0x2BA01477u;
    case 0x4: return s->ctrl_stat | ((s->ctrl_stat & (CS_CDBGPWRUPREQ | CS_CSYSPWRUPREQ)) << 1);
    case 0x8: return 0;
    default: return s->rdbuff;
    }
}

static void dp_write(swd_sim_t *s, uint32_t a, uint32_t v)
{
    switch (a) {
    case 0x0:
        if (v & (1u << 1)) s->ctrl_stat &= ~CS_STICKYCMP;
        if (v & (1u << 2)) s->ctrl_stat &= ~CS_STICKYERR;
        if (v & (1u << 3)) s->ctrl_stat &= ~CS_WDATAERR;
        if (v & (1u << 4)) s->ctrl_stat &= ~CS_STICKYORUN;
        break;
    case 0x4: s->ctrl_stat = (s->ctrl_stat & CS_STICKY) | (v & ~CS_STICKY & ~0xA0000000u); break;
    case 0x8: s->select = v; break;
    default: break;
    }
}

static int ap_read(swd_sim_t *s, uint32_t ap, uint32_t reg, uint32_t *v)
{
    if (ap == 0) {
        if (!s->approtect_open) {
            if (reg == 0xFC) { *v = 0x23000000u; return 1; }   /* as seen on a locked nRF52840 */
            return 0;
        }
        switch (reg) {
        case 0x00: *v = s->csw | 0x40u; return 1;
        case 0x04: *v = s->tar; return 1;
        case 0x0C: {
            int ok = mem_read(s, s->tar, v);
            if ((s->csw & 0x30u) == 0x10u) s->tar = (s->tar & ~0x3FFu) | ((s->tar + 4u) & 0x3FFu);
            return ok;
        }
        case 0xFC: *v = 0x24770011u; return 1;
        default: *v = 0; return 1;
        }
    }
    if (ap == 1) {
        switch (reg) {
        case 0x000: *v = s->ctrlap_reset; return 1;
        case 0x008:
            if (s->eraseall_polls) { s->eraseall_polls--; *v = 1; } else *v = 0;
            return 1;
        case 0x00C: *v = s->approtect_open; return 1;
        case 0x0FC: *v = 0x02880000u; return 1;
        default: *v = 0; return 1;
        }
    }
    *v = 0;   /* no such AP: IDR reads as zero */
    return 1;
}

static int ap_write(swd_sim_t *s, uint32_t ap, uint32_t reg, uint32_t v)
{
    if (ap == 0) {
        if (!s->approtect_open) return 0;
        switch (reg) {
        case 0x00: s->csw = v; return 1;
        case 0x04: s->tar = v; return 1;
        case 0x0C: {
            int ok = mem_write(s, s->tar, v);
            if ((s->csw & 0x30u) == 0x10u) s->tar = (s->tar & ~0x3FFu) | ((s->tar + 4u) & 0x3FFu);
            return ok;
        }
        default: return 1;
        }
    }
    if (ap == 1) {
        switch (reg) {
        case 0x000:
            if (s->ctrlap_reset && !(v & 1u)) do_reset(s);
            s->ctrlap_reset = v & 1u;
            return 1;
        case 0x004:
            if (v & 1u) {
                memset(s->flash, 0xFF, sizeof(s->flash));
                memset(s->uicr, 0xFF, sizeof(s->uicr));
                memset(s->ram, 0, sizeof(s->ram));
                s->approtect_open = 1;
                s->eraseall_polls = 3;
            }
            return 1;
        default: return 1;
        }
    }
    return 1;
}

/* Called after the 8 request bits: decide the ACK and, for reads, the data. */
static uint32_t respond(swd_sim_t *s)
{
    if (!s->apndp) {
        if (s->rnw) s->data = dp_read(s, s->addr);
        return 1;
    }
    s->ap_accesses++;
    step_resident(s);
    if (s->inject_wait) { s->inject_wait--; return 2; }
    if (s->ctrl_stat & CS_STICKY) return 4;
    uint32_t ap = s->select >> 24, reg = (s->select & 0xF0u) | s->addr;
    if (ap == 0 && !(s->ctrl_stat & CS_CDBGPWRUPREQ)) { s->ctrl_stat |= CS_STICKYERR; return 4; }
    /* AHB stall: a flash/UICR write while the NVMC is still busy waits on the bus -> WAIT on the wire */
    if (ap == 0 && !s->rnw && reg == 0x0C && s->nvmc_busy &&
        (s->tar < SIM_FLASH_SIZE || (s->tar >= SIM_UICR_BASE && s->tar < SIM_UICR_BASE + 0x1000u))) {
        s->nvmc_busy--;
        s->stalls++;
        return 2;
    }
    if (s->rnw) {
        uint32_t v = 0;
        int ok = ap_read(s, ap, reg, &v);
        s->data = s->rdbuff;   /* posted: return the previous result */
        s->rdbuff = ok ? v : 0;
        if (!ok) s->ctrl_stat |= CS_STICKYERR;
    }
    return 1;
}

static void do_write(swd_sim_t *s)
{
    if (!s->apndp) {
        dp_write(s, s->addr, s->data);
        return;
    }
    uint32_t ap = s->select >> 24, reg = (s->select & 0xF0u) | s->addr;
    if (!ap_write(s, ap, reg, s->data)) s->ctrl_stat |= CS_STICKYERR;
}

/* ---- wire ------------------------------------------------------------------------------------------------ */

static void on_edge(swd_sim_t *s)
{
    int b = s->host_drives ? (int)s->host_level : -1;
    if (s->host_drives && s->tgt_out >= 0) s->contention++;
    s->edges++;
    if (s->absent) { s->tgt_out = -1; return; }
    switch (s->state) {
    case S_IDLE:
        s->tgt_out = -1;
        s->ones = (b == 1) ? s->ones + 1 : 0;
        if (s->ones >= 50) { s->in_reset = 1; s->lockout = 1; }
        if (s->in_reset) { if (b == 0) s->in_reset = 0; return; }
        if (b == 1) { s->req = 1; s->k = 1; s->state = S_REQ; }
        return;
    case S_REQ: {
        s->ones = (b == 1) ? s->ones + 1 : 0;
        s->req |= (uint32_t)(b == 1) << s->k;
        if (++s->k < 8) return;
        s->state = S_IDLE;
        s->tgt_out = -1;
        uint32_t r = s->req, a2 = (r >> 3) & 1u, a3 = (r >> 4) & 1u;
        s->apndp = (r >> 1) & 1u;
        s->rnw = (r >> 2) & 1u;
        if (((r >> 6) & 1u) || !((r >> 7) & 1u) || ((r >> 5) & 1u) != ((s->apndp + s->rnw + a2 + a3) & 1u))
            return;   /* malformed request: no response */
        s->addr = (a2 << 2) | (a3 << 3);
        if (s->lockout && !(s->apndp == 0 && s->rnw == 1 && s->addr == 0))
            return;   /* lock-out after line reset until DPIDR is read */
        s->ack = respond(s);
        s->k = 0;
        s->state = S_ACK;
        return;
    }
    case S_ACK:
        s->tgt_out = (int32_t)((s->ack >> s->k) & 1u);
        if (++s->k < 3) return;
        s->k = 0;
        s->state = (s->ack == 1) ? (s->rnw ? S_RDATA : S_WTRN) : S_RELEASE;
        return;
    case S_RDATA:
        s->tgt_out = (int32_t)((s->data >> s->k) & 1u);
        if (++s->k == 32) s->state = S_RPAR;
        return;
    case S_RPAR: {
        uint32_t p = esw_parity32(s->data);
        if (s->inject_parity) { s->inject_parity--; p ^= 1u; }
        s->tgt_out = (int32_t)p;
        s->state = S_RELEASE;
        return;
    }
    case S_RELEASE:
        s->tgt_out = -1;
        s->ones = 0;
        s->state = S_IDLE;
        return;
    case S_WTRN:
        s->tgt_out = -1;
        if (++s->k == 2) { s->k = 0; s->data = 0; s->state = S_WDATA; }
        return;
    case S_WDATA:
        s->data |= (uint32_t)(b == 1) << s->k;
        if (++s->k == 32) s->state = S_WPAR;
        return;
    case S_WPAR:
        s->state = S_IDLE;
        if ((uint32_t)(b == 1) != esw_parity32(s->data)) { s->ctrl_stat |= CS_WDATAERR; return; }
        do_write(s);
        return;
    default:
        s->state = S_IDLE;
        return;
    }
}

static void p_swclk(void *ctx, uint32_t level)
{
    swd_sim_t *s = ctx;
    if (!s->clk && level) on_edge(s);
    s->clk = level ? 1u : 0u;
}

static void p_swdio(void *ctx, uint32_t level)
{
    ((swd_sim_t *)ctx)->host_level = level & 1u;
}

static uint32_t p_read(void *ctx)
{
    swd_sim_t *s = ctx;
    return s->tgt_out >= 0 ? (uint32_t)s->tgt_out : 1u;   /* tool-side pull-up when nobody drives */
}

static void p_drive(void *ctx, uint32_t drive)
{
    swd_sim_t *s = ctx;
    s->host_drives = drive ? 1u : 0u;
    if (drive && s->tgt_out >= 0) s->contention++;
}

const esw_bitbang_pins_t swd_sim_pins = { p_swclk, p_swdio, p_read, p_drive, 0 };

void swd_sim_init(swd_sim_t *s, uint32_t locked)
{
    memset(s, 0, sizeof(*s));
    s->tgt_out = -1;
    s->resident = -1;
    memset(s->flash, 0xFF, sizeof(s->flash));
    memset(s->uicr, 0xFF, sizeof(s->uicr));
    memset(s->ext, 0xFF, sizeof(s->ext));
    s->approtect_open = locked ? 0u : 1u;
    s->lockout = 1;
}

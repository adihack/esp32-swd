/* SPDX-License-Identifier: MIT */
/* Bit-level SWD target simulator: an nRF52840-like chip behind the esw bit-bang pin interface.
 *
 * Wire protocol (request, turnaround, ACK, data, parity), SW-DP (lock-out, power-up, sticky flags, posted AP
 * reads), an AHB-AP with a 1 KB auto-increment wrap, a Nordic CTRL-AP (APPROTECT, ERASEALL, RESET), NVMC flash
 * semantics, Cortex-M debug registers, and "native code" hooks: when the core is resumed at a hooked address the
 * simulator runs a C function in place of target code (used to emulate RAM loaders, including a streaming loader
 * that keeps running while the probe accesses memory). External flash at SIM_EXT_BASE is reachable only by hooks.
 */
#ifndef SWD_SIM_H
#define SWD_SIM_H
#include <stdint.h>
#include "esw/swd_bitbang.h"

#define SIM_FLASH_SIZE   (64u * 1024u)
#define SIM_RAM_BASE     0x20000000u
#define SIM_RAM_SIZE     (64u * 1024u)
#define SIM_UICR_BASE    0x10001000u
#define SIM_EXT_BASE     0x90000000u
#define SIM_EXT_SIZE     (64u * 1024u)
#define SIM_EXT_SECTOR   4096u

typedef struct swd_sim swd_sim_t;

typedef enum { SIM_HOOK_CALL, SIM_HOOK_RESIDENT } sim_hook_kind_t;
/* CALL hooks run once and return a value in R0. RESIDENT hooks are stepped on every probe AP access and return
 * nonzero when they exit (core then halts at LR, like returning into a BKPT). */
typedef uint32_t (*sim_hook_fn)(swd_sim_t *s, uint32_t *regs, int first);

typedef struct {
    uint32_t addr;
    sim_hook_kind_t kind;
    sim_hook_fn fn;
} sim_hook_t;

struct swd_sim {
    /* wire */
    uint32_t clk, host_level, host_drives, edges, contention;
    int32_t tgt_out;   /* -1 released */
    /* protocol */
    int state;
    uint32_t k, ones, in_reset, lockout, req, ack, data, apndp, rnw, addr;
    /* DP */
    uint32_t ctrl_stat, select, rdbuff;
    /* AHB-AP */
    uint32_t csw, tar;
    /* chip */
    uint32_t approtect_open, eraseall_polls, ctrlap_reset;
    uint32_t nvmc_config, nvmc_busy;
    uint32_t halted, debugen, dcrdr, demcr, dfsr, regs[24], reset_st;
    uint32_t gpio_out;
    uint32_t flash[SIM_FLASH_SIZE / 4], uicr[1024], ram[SIM_RAM_SIZE / 4], ext[SIM_EXT_SIZE / 4];
    sim_hook_t hooks[8];
    uint32_t nhooks;
    int resident;          /* index of the running resident hook, -1 none */
    /* fault injection and counters */
    uint32_t absent, inject_wait, inject_parity, nvmc_busy_reads;
    uint32_t ap_accesses, resets, lose_reset_st, stalls;   /* lose_reset_st: S_RESET_ST consumed by an unseen read */
    uint32_t last_ap_wait_free;   /* stats */
};

void swd_sim_init(swd_sim_t *s, uint32_t locked);
void swd_sim_add_hook(swd_sim_t *s, uint32_t addr, sim_hook_kind_t kind, sim_hook_fn fn);
extern const esw_bitbang_pins_t swd_sim_pins;

/* target-side memory helpers for hooks (no faults, direct) */
uint32_t sim_ram_rd(swd_sim_t *s, uint32_t addr);
void sim_ram_wr(swd_sim_t *s, uint32_t addr, uint32_t v);

#endif

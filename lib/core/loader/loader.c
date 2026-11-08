/* SPDX-License-Identifier: MIT */
#include "esw/loader.h"
#include <string.h>

#define MB(l, off) ((l)->d->mailbox_addr + (off))
#define SLOT(l, i, w) ((l)->d->mailbox_addr + ESW_LDR_MB_SLOTS + ((i) * ESW_LDR_SLOT_WORDS + (w)) * 4u)

esw_err_t esw_loader_load(esw_loader_t *l, esw_cpu_t *cpu, const esw_clock_t *clk, const esw_loader_desc_t *d)
{
    if (!d || !d->blob || !d->blob_words || !d->stack_top || !d->trap_addr)
        return ESW_ERR_ARG;
    memset(l, 0, sizeof(*l));
    l->cpu = cpu;
    l->clk = clk;
    l->d = d;
    ESW_TRY(cpu->ops->halt(cpu));
    ESW_TRY(esw_mem_write_block(cpu->mem, d->load_addr, d->blob, d->blob_words));
    /* read back: a loader that is not what we wrote must never run */
    for (uint32_t i = 0; i < d->blob_words; i++) {
        uint32_t v = 0;
        ESW_TRY(esw_mem_read32(cpu->mem, d->load_addr + 4u * i, &v));
        if (v != d->blob[i])
            return ESW_ERR_VERIFY;
    }
    l->loaded = 1;
    return ESW_OK;
}

static esw_err_t wait_halt(esw_loader_t *l, uint32_t timeout_ms)
{
    uint32_t t0 = esw_now_ms(l->clk);
    for (;;) {
        uint32_t halted = 0;
        ESW_TRY(l->cpu->ops->is_halted(l->cpu, &halted));
        if (halted)
            return ESW_OK;
        if (esw_elapsed_ms(l->clk, t0) >= timeout_ms) {
            (void)l->cpu->ops->halt(l->cpu);
            return ESW_ERR_TIMEOUT;
        }
        esw_sleep_ms(l->clk, 1);
    }
}

static esw_err_t guard(esw_loader_t *l, uint32_t on)
{
    return l->cpu->ops->call_guard ? l->cpu->ops->call_guard(l->cpu, on) : ESW_OK;
}

esw_err_t esw_loader_call(esw_loader_t *l, uint32_t fn, const uint32_t args[4], uint32_t timeout_ms, uint32_t *ret)
{
    if (!l->loaded || l->streaming || !fn)
        return ESW_ERR_STATE;
    const esw_loader_desc_t *d = l->d;
    ESW_TRY(l->cpu->ops->prepare_call(l->cpu, fn, args, d->stack_top, d->static_base, d->trap_addr));
    ESW_TRY(guard(l, 1));   /* a reset or fault inside the loader halts at once: no target firmware runs */
    esw_err_t e = l->cpu->ops->resume(l->cpu);
    if (e == ESW_OK)
        e = wait_halt(l, timeout_ms ? timeout_ms : d->timeout_call_ms);
    esw_err_t g = guard(l, 0);
    ESW_TRY(e);
    ESW_TRY(g);
    uint32_t pc = 0, r0 = 0;
    ESW_TRY(l->cpu->ops->reg_read(l->cpu, ESW_REG_PC, &pc));
    if ((pc & ~1u) != (d->trap_addr & ~1u))
        return ESW_ERR_LOADER;   /* stopped somewhere else: fault, foreign breakpoint */
    ESW_TRY(l->cpu->ops->reg_read(l->cpu, ESW_REG_ARG0, &r0));
    if (ret)
        *ret = r0;
    return ESW_OK;
}

esw_err_t esw_loader_call_program(esw_loader_t *l, uint32_t addr, const uint32_t *data, uint32_t nwords)
{
    const esw_loader_desc_t *d = l->d;
    if (!d->fn_program_page || !d->page_buf_addr || !d->page_size || (d->page_size & 3u))
        return ESW_ERR_UNSUPPORTED;
    uint32_t page_words = d->page_size / 4u;
    while (nwords) {
        uint32_t n = nwords < page_words ? nwords : page_words;
        ESW_TRY(esw_mem_write_block(l->cpu->mem, d->page_buf_addr, data, n));
        uint32_t args[4] = { addr, n * 4u, d->page_buf_addr, 0 };
        uint32_t r = 0;
        ESW_TRY(esw_loader_call(l, d->fn_program_page, args, 0, &r));
        if (r != 0) {
            l->last_status = r;
            return ESW_ERR_LOADER;
        }
        addr += n * 4u;
        data += n;
        nwords -= n;
    }
    return ESW_OK;
}

/* ---- streaming ABI ------------------------------------------------------------------------------------- */

esw_err_t esw_loader_stream_start(esw_loader_t *l)
{
    const esw_loader_desc_t *d = l->d;
    if (!l->loaded || !d->fn_stream || !d->mailbox_addr)
        return ESW_ERR_UNSUPPORTED;
    ESW_TRY(esw_mem_write32(l->cpu->mem, MB(l, ESW_LDR_MB_MAGIC), 0));
    uint32_t args[4] = { d->mailbox_addr, 0, 0, 0 };
    ESW_TRY(l->cpu->ops->prepare_call(l->cpu, d->fn_stream, args, d->stack_top, d->static_base, d->trap_addr));
    ESW_TRY(guard(l, 1));   /* stays on while the resident loader runs; esw_loader_stream_stop turns it off */
    ESW_TRY(l->cpu->ops->resume(l->cpu));
    uint32_t t0 = esw_now_ms(l->clk), magic = 0;
    for (;;) {
        ESW_TRY(esw_mem_read32(l->cpu->mem, MB(l, ESW_LDR_MB_MAGIC), &magic));
        if (magic == ESW_LDR_MAGIC)
            break;
        if (esw_elapsed_ms(l->clk, t0) >= d->timeout_call_ms) {
            (void)l->cpu->ops->halt(l->cpu);
            return ESW_ERR_TIMEOUT;
        }
        esw_sleep_ms(l->clk, 1);
    }
    uint32_t hdr[6];
    ESW_TRY(esw_mem_read_block(l->cpu->mem, MB(l, ESW_LDR_MB_MAGIC), hdr, 6));
    if (hdr[1] != ESW_LDR_ABI_VERSION || hdr[3] == 0 || hdr[4] == 0 || (hdr[4] & 3u))
        return ESW_ERR_LOADER;
    l->caps = hdr[2];
    l->nbuf = hdr[3];
    l->buf_size = hdr[4];
    l->buf_base = hdr[5];
    ESW_TRY(esw_mem_read32(l->cpu->mem, MB(l, ESW_LDR_MB_DONE_SEQ), &l->cmd_seq));
    l->next_slot = 0;
    l->streaming = 1;
    return ESW_OK;
}

esw_err_t esw_loader_stream_cmd(esw_loader_t *l, esw_ldr_cmd_t cmd, uint32_t addr, uint32_t len, uint32_t arg,
                                uint32_t timeout_ms, uint32_t *result)
{
    if (!l->streaming)
        return ESW_ERR_STATE;
    uint32_t params[4] = { (uint32_t)cmd, addr, len, arg };
    ESW_TRY(esw_mem_write_block(l->cpu->mem, MB(l, ESW_LDR_MB_CMD), params, 4));
    l->cmd_seq++;
    ESW_TRY(esw_mem_write32(l->cpu->mem, MB(l, ESW_LDR_MB_CMD_SEQ), l->cmd_seq));
    uint32_t t0 = esw_now_ms(l->clk), done = 0;
    if (!timeout_ms)
        timeout_ms = l->d->timeout_call_ms;
    for (;;) {
        ESW_TRY(esw_mem_read32(l->cpu->mem, MB(l, ESW_LDR_MB_DONE_SEQ), &done));
        if (done == l->cmd_seq)
            break;
        if (esw_elapsed_ms(l->clk, t0) >= timeout_ms)
            return ESW_ERR_TIMEOUT;
        esw_sleep_ms(l->clk, 1);
    }
    uint32_t sr[2];
    ESW_TRY(esw_mem_read_block(l->cpu->mem, MB(l, ESW_LDR_MB_STATUS), sr, 2));
    l->last_status = sr[0];
    if (result)
        *result = sr[1];
    return sr[0] == 0 ? ESW_OK : ESW_ERR_LOADER;
}

/* Wait until slot i is FREE or DONE; returns the slot status of a finished job in *status. */
static esw_err_t slot_reclaim(esw_loader_t *l, uint32_t i, uint32_t *status)
{
    uint32_t t0 = esw_now_ms(l->clk), st = 0;
    *status = 0;
    for (;;) {
        ESW_TRY(esw_mem_read32(l->cpu->mem, SLOT(l, i, 0), &st));
        if (st == ESW_LDR_SLOT_FREE)
            return ESW_OK;
        if (st == ESW_LDR_SLOT_DONE) {
            ESW_TRY(esw_mem_read32(l->cpu->mem, SLOT(l, i, 3), status));
            return esw_mem_write32(l->cpu->mem, SLOT(l, i, 0), ESW_LDR_SLOT_FREE);
        }
        if (esw_elapsed_ms(l->clk, t0) >= l->d->timeout_call_ms)
            return ESW_ERR_TIMEOUT;
        esw_sleep_ms(l->clk, 0);
    }
}

esw_err_t esw_loader_stream_program(esw_loader_t *l, uint32_t addr, const uint32_t *data, uint32_t nwords)
{
    if (!l->streaming)
        return ESW_ERR_STATE;
    uint32_t buf_words = l->buf_size / 4u, st = 0;
    while (nwords) {
        uint32_t i = l->next_slot;
        ESW_TRY(slot_reclaim(l, i, &st));
        if (st) {
            l->last_status = st;
            return ESW_ERR_LOADER;
        }
        uint32_t n = nwords < buf_words ? nwords : buf_words;
        ESW_TRY(esw_mem_write_block(l->cpu->mem, l->buf_base + i * l->buf_size, data, n));
        uint32_t desc[3] = { addr, n * 4u, 0 };
        ESW_TRY(esw_mem_write_block(l->cpu->mem, SLOT(l, i, 1), desc, 3));
        ESW_TRY(esw_mem_write32(l->cpu->mem, SLOT(l, i, 0), ESW_LDR_SLOT_FILLED));   /* hand over last */
        l->next_slot = (i + 1u) % l->nbuf;
        addr += n * 4u;
        data += n;
        nwords -= n;
    }
    /* drain: every slot must come back DONE with status 0 */
    for (uint32_t k = 0; k < l->nbuf; k++) {
        ESW_TRY(slot_reclaim(l, k, &st));
        if (st) {
            l->last_status = st;
            return ESW_ERR_LOADER;
        }
    }
    return ESW_OK;
}

esw_err_t esw_loader_stream_stop(esw_loader_t *l)
{
    if (!l->streaming)
        return ESW_OK;
    l->streaming = 0;
    uint32_t params[4] = { ESW_LDR_CMD_EXIT, 0, 0, 0 };
    ESW_TRY(esw_mem_write_block(l->cpu->mem, MB(l, ESW_LDR_MB_CMD), params, 4));
    l->cmd_seq++;
    ESW_TRY(esw_mem_write32(l->cpu->mem, MB(l, ESW_LDR_MB_CMD_SEQ), l->cmd_seq));
    esw_err_t e = wait_halt(l, l->d->timeout_call_ms);
    esw_err_t g = guard(l, 0);
    if (e == ESW_ERR_TIMEOUT)
        e = ESW_OK;   /* halted by wait_halt: the loader did not exit by itself, but the core is stopped */
    return e != ESW_OK ? e : g;
}

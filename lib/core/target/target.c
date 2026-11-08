/* SPDX-License-Identifier: MIT */
#include "esw/target.h"
#include "esw/cortexm.h"
#include <string.h>

esw_err_t esw_target_detect(esw_target_t *t, esw_dap_t *dap, const esw_clock_t *clk)
{
    memset(t, 0, sizeof(*t));
    t->dap = dap;
    t->clk = clk;
    for (const esw_target_driver_t *const *p = esw_target_drivers; *p; p++) {
        t->drv = *p;
        if (t->drv->probe(t) == ESW_OK)
            return t->drv->attach(t);
    }
    t->drv = NULL;
    return ESW_ERR_NOT_FOUND;
}

esw_err_t esw_target_add_region(esw_target_t *t, const esw_mem_region_t *r)
{
    if (t->nregions >= ESW_TARGET_MAX_REGIONS)
        return ESW_ERR_ARG;
    t->regions[t->nregions++] = *r;
    return ESW_OK;
}

const esw_mem_region_t *esw_target_region(const esw_target_t *t, uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < t->nregions; i++) {
        const esw_mem_region_t *r = &t->regions[i];
        if (addr >= r->start && len <= r->size && addr - r->start <= r->size - len)
            return r;
    }
    return NULL;
}

/* Make sure the region's loader is downloaded and (for streaming) running. */
static esw_err_t loader_for(esw_target_t *t, const esw_mem_region_t *r)
{
    if (t->loader_active == r->loader && t->loader.loaded)
        return ESW_OK;
    ESW_TRY(esw_target_finish(t));
    ESW_TRY(esw_loader_load(&t->loader, &t->cpu, t->clk, r->loader));
    t->loader_active = r->loader;
    if (r->loader->fn_init) {
        uint32_t args[4] = { r->start, 0, 0, 0 }, ret = 0;
        ESW_TRY(esw_loader_call(&t->loader, r->loader->fn_init, args, 0, &ret));
        if (ret)
            return ESW_ERR_LOADER;
    }
    if (r->use_stream)
        ESW_TRY(esw_loader_stream_start(&t->loader));
    return ESW_OK;
}

esw_err_t esw_target_finish(esw_target_t *t)
{
    if (!t->loader_active)
        return ESW_OK;
    esw_err_t e = esw_loader_stream_stop(&t->loader);
    if (e == ESW_OK && t->loader_active->fn_uninit && t->loader.loaded) {
        uint32_t args[4] = { 0, 0, 0, 0 };
        e = esw_loader_call(&t->loader, t->loader_active->fn_uninit, args, 0, NULL);
    }
    t->loader_active = NULL;
    t->loader.loaded = 0;
    return e;
}

esw_err_t esw_target_erase(esw_target_t *t, uint32_t addr, uint32_t len)
{
    const esw_mem_region_t *r = esw_target_region(t, addr, len);
    if (!r)
        return ESW_ERR_NOT_FOUND;
    if (!r->loader)
        return t->drv->erase ? t->drv->erase(t, r, addr, len) : ESW_ERR_UNSUPPORTED;
    ESW_TRY(loader_for(t, r));
    if (r->use_stream)
        return esw_loader_stream_cmd(&t->loader, ESW_LDR_CMD_ERASE, addr, len, 0, r->loader->timeout_erase_ms, NULL);
    if (!r->loader->fn_erase_sector || !r->sector_size)
        return ESW_ERR_UNSUPPORTED;
    for (uint32_t a = addr - (addr - r->start) % r->sector_size; a < addr + len; a += r->sector_size) {
        uint32_t args[4] = { a, 0, 0, 0 }, ret = 0;
        ESW_TRY(esw_loader_call(&t->loader, r->loader->fn_erase_sector, args, r->loader->timeout_erase_ms, &ret));
        if (ret)
            return ESW_ERR_LOADER;
    }
    return ESW_OK;
}

esw_err_t esw_target_program(esw_target_t *t, uint32_t addr, const uint32_t *data, uint32_t nwords)
{
    const esw_mem_region_t *r = esw_target_region(t, addr, nwords * 4u);
    if (!r || (addr & 3u))
        return r ? ESW_ERR_ARG : ESW_ERR_NOT_FOUND;
    if (!r->loader)
        return t->drv->program ? t->drv->program(t, r, addr, data, nwords) : ESW_ERR_UNSUPPORTED;
    ESW_TRY(loader_for(t, r));
    return r->use_stream ? esw_loader_stream_program(&t->loader, addr, data, nwords)
                         : esw_loader_call_program(&t->loader, addr, data, nwords);
}

/* CRC-32/ISO-HDLC (zlib.crc32), the same CRC the loaders compute. crc is the running value (0 to start). */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t crc32_words(const uint32_t *w, uint32_t n)
{
    uint32_t crc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b[4] = { (uint8_t)w[i], (uint8_t)(w[i] >> 8), (uint8_t)(w[i] >> 16), (uint8_t)(w[i] >> 24) };
        crc = crc32_update(crc, b, 4);
    }
    return crc;
}

esw_err_t esw_target_crc32(esw_target_t *t, uint32_t addr, uint32_t len, uint32_t flags, uint32_t *crc)
{
    const esw_mem_region_t *r = esw_target_region(t, addr, len);
    if (!r || (addr & 3u))
        return r ? ESW_ERR_ARG : ESW_ERR_NOT_FOUND;
    if (r->loader && r->use_stream) {
        ESW_TRY(loader_for(t, r));
        if (!(t->loader.caps & ESW_LDR_CAP_CRC32))
            return ESW_ERR_UNSUPPORTED;
        return esw_loader_stream_cmd(&t->loader, ESW_LDR_CMD_CRC32, addr, len, 0, 0, crc);
    }
    if (r->kind == ESW_MEM_EXT)
        return ESW_ERR_UNSUPPORTED;   /* not memory-mapped for the debugger without a loader */
    ESW_TRY(esw_target_finish(t));    /* the CRC loader reuses scratch RAM an active loader may occupy */
    if ((flags & ESW_CRC_RESET_HALT) && t->drv->reset)
        ESW_TRY(t->drv->reset(t, ESW_RESET_HALT));
    /* the loader occupies [scratch, scratch + ESW_CRC32_LDR_RAM): a range touching it must be read back instead,
     * or the result would be the loader's own CRC and the caller's data would be overwritten */
    uint32_t overlaps = t->scratch_ram && addr < t->scratch_ram + ESW_CRC32_LDR_RAM && t->scratch_ram < addr + len;
    if (t->scratch_ram && !overlaps && esw_loader_crc32(&t->cpu, t->clk, t->scratch_ram, addr, len, crc) == ESW_OK)
        return ESW_OK;
    /* read-back fallback */
    uint32_t buf[256], c = 0;
    while (len) {
        uint32_t nbytes = len < sizeof(buf) ? len : (uint32_t)sizeof(buf);
        uint32_t nwords = (nbytes + 3u) / 4u;
        ESW_TRY(esw_mem_read_block(&t->mem, addr, buf, nwords));
        uint8_t bytes[sizeof(buf)];
        for (uint32_t i = 0; i < nwords; i++)
            for (uint32_t b = 0; b < 4; b++)
                bytes[4u * i + b] = (uint8_t)(buf[i] >> (8u * b));
        c = crc32_update(c, bytes, nbytes);
        addr += nbytes;
        len -= nbytes;
    }
    *crc = c;
    return ESW_OK;
}

esw_err_t esw_target_verify(esw_target_t *t, uint32_t addr, const uint32_t *data, uint32_t nwords)
{
    const esw_mem_region_t *r = esw_target_region(t, addr, nwords * 4u);
    if (!r)
        return ESW_ERR_NOT_FOUND;
    if (r->loader && r->use_stream) {
        ESW_TRY(loader_for(t, r));
        if (t->loader.caps & ESW_LDR_CAP_CRC32) {
            uint32_t crc = 0;
            ESW_TRY(esw_loader_stream_cmd(&t->loader, ESW_LDR_CMD_CRC32, addr, nwords * 4u, 0, 0, &crc));
            return crc == crc32_words(data, nwords) ? ESW_OK : ESW_ERR_VERIFY;
        }
    }
    if (r->kind == ESW_MEM_EXT)
        return ESW_ERR_UNSUPPORTED;   /* not memory-mapped for the debugger without a loader */
    uint32_t buf[256];   /* one auto-increment burst per 1 KB */
    while (nwords) {
        uint32_t n = nwords < 256u ? nwords : 256u;
        ESW_TRY(esw_mem_read_block(&t->mem, addr, buf, n));
        if (memcmp(buf, data, n * 4u) != 0)
            return ESW_ERR_VERIFY;
        addr += n * 4u;
        data += n;
        nwords -= n;
    }
    return ESW_OK;
}

/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "esw/loader.h"
#include "crc32_blob.inc"

esw_err_t esw_loader_crc32(esw_cpu_t *cpu, const esw_clock_t *clk, uint32_t scratch, uint32_t addr, uint32_t len,
                           uint32_t *crc)
{
    if ((scratch & 3u) || (addr & 3u))
        return ESW_ERR_ARG;
    esw_loader_desc_t d = { 0 };
    d.load_addr = scratch;
    d.blob = esw_crc32_ldr_blob;
    d.blob_words = ESW_CRC32_LDR_WORDS;
    d.stack_top = scratch + ESW_CRC32_LDR_RAM;
    d.trap_addr = scratch + ESW_CRC32_LDR_OFF_TRAP;
    d.timeout_call_ms = 1000;
    esw_loader_t l;
    ESW_TRY(esw_loader_load(&l, cpu, clk, &d));   /* halts the core, downloads and read-back-checks the blob */
    const uint32_t tab = scratch + 0x400u;
    uint32_t args[4] = { tab, 0, 0, 0 };
    ESW_TRY(esw_loader_call(&l, scratch + ESW_CRC32_LDR_OFF_INIT, args, 1000, NULL));
    uint32_t c = 0;
    while (len) {
        uint32_t n = len < ESW_CRC32_CHUNK ? len : ESW_CRC32_CHUNK;
        args[0] = addr;
        args[1] = n;
        args[2] = tab;
        args[3] = c;
        /* ~33 ms per chunk on a 64 MHz M4; generous for slow-clocked parts */
        ESW_TRY(esw_loader_call(&l, scratch + ESW_CRC32_LDR_OFF_UPDATE, args, 5000, &c));
        addr += n;
        len -= n;
    }
    *crc = c;
    return ESW_OK;
}

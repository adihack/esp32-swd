/* SPDX-License-Identifier: MIT */
/* esp-swd: generic target-side RAM loaders (common stack, any chip).
 *
 * A loader is code that the probe downloads into target RAM and executes through the core-control interface.
 * Chip modules only supply descriptors; this runtime does the download, execution and communication.
 *
 *  - Call ABI (CMSIS flash algorithm / FLM, SEGGER Open Flashloader compatible): every operation is a function
 *    call fn(a0..a3) that returns to a breakpoint ("trap"). Simple, compatible, but halts once per call.
 *  - Streaming ABI (esp-swd native): the loader is started once and services a mailbox and a ring of buffers in
 *    target RAM while the core runs. The probe fills the next buffer while the loader programs the previous one;
 *    no halt/resume per page.
 *
 * The host parses the ELF and sends a normalized descriptor; the probe has no ELF parser.
 */
#ifndef ESW_LOADER_H
#define ESW_LOADER_H
#include <stdint.h>
#include "esw/err.h"
#include "esw/clock.h"
#include "esw/cpu.h"

/* ---- streaming-ABI mailbox layout in target RAM (word offsets from mailbox_addr) --------------------- */
#define ESW_LDR_MAGIC        0x4C575345u   /* "ESWL": written by the loader when its loop runs */
#define ESW_LDR_ABI_VERSION  1u
#define ESW_LDR_MB_MAGIC     0x00u
#define ESW_LDR_MB_VERSION   0x04u
#define ESW_LDR_MB_CAPS      0x08u
#define ESW_LDR_MB_NBUF      0x0Cu
#define ESW_LDR_MB_BUFSIZE   0x10u   /* bytes per buffer */
#define ESW_LDR_MB_BUFBASE   0x14u   /* address of buffer 0; buffers are contiguous */
#define ESW_LDR_MB_CMD_SEQ   0x18u   /* probe increments after writing a command */
#define ESW_LDR_MB_DONE_SEQ  0x1Cu   /* loader copies CMD_SEQ when the command finished */
#define ESW_LDR_MB_CMD       0x20u
#define ESW_LDR_MB_ADDR      0x24u
#define ESW_LDR_MB_LEN       0x28u
#define ESW_LDR_MB_ARG       0x2Cu
#define ESW_LDR_MB_STATUS    0x30u   /* 0 = OK, else loader-specific error */
#define ESW_LDR_MB_RESULT    0x34u
#define ESW_LDR_MB_SLOTS     0x40u   /* nbuf slots of 4 words: state, addr, len, status */
#define ESW_LDR_SLOT_WORDS   4u

#define ESW_LDR_SLOT_FREE    0u
#define ESW_LDR_SLOT_FILLED  1u   /* probe -> loader: program buffer i to slot.addr, slot.len bytes */
#define ESW_LDR_SLOT_BUSY    2u
#define ESW_LDR_SLOT_DONE    3u   /* loader -> probe: slot.status valid */

typedef enum {
    ESW_LDR_CMD_NOP = 0,
    ESW_LDR_CMD_ERASE = 1,        /* addr, len */
    ESW_LDR_CMD_CHIP_ERASE = 2,
    ESW_LDR_CMD_CRC32 = 3,        /* addr, len -> result (CRC-32/ISO-HDLC) */
    ESW_LDR_CMD_READ = 4,         /* addr, len -> buffer arg */
    ESW_LDR_CMD_EXIT = 0xFF,      /* loader returns to the trap (core halts) */
} esw_ldr_cmd_t;

#define ESW_LDR_CAP_ERASE       (1u << 0)
#define ESW_LDR_CAP_CHIP_ERASE  (1u << 1)
#define ESW_LDR_CAP_PROGRAM     (1u << 2)
#define ESW_LDR_CAP_CRC32       (1u << 3)
#define ESW_LDR_CAP_READ        (1u << 4)

/* ---- descriptor -------------------------------------------------------------------------------------- */
typedef struct {
    uint32_t load_addr;            /* RAM address of the blob */
    const uint32_t *blob;          /* code + data, little-endian words */
    uint32_t blob_words;
    uint32_t stack_top;            /* SP for every call */
    uint32_t static_base;          /* SB / R9 for position-independent code, 0 if unused */
    uint32_t trap_addr;            /* address of a breakpoint instruction inside the blob */
    /* call ABI entry points (0 = absent) */
    uint32_t fn_init, fn_uninit, fn_erase_sector, fn_erase_chip, fn_program_page, fn_verify;
    uint32_t page_buf_addr;        /* RAM buffer for ProgramPage data (call ABI) */
    uint32_t page_size;            /* bytes per ProgramPage call */
    /* streaming ABI (0 = absent) */
    uint32_t fn_stream;            /* resident loop entry: fn_stream(mailbox_addr) */
    uint32_t mailbox_addr;
    /* timeouts */
    uint32_t timeout_call_ms;      /* default per call / per command */
    uint32_t timeout_erase_ms;     /* erase sector or range */
    uint32_t timeout_chip_erase_ms;
} esw_loader_desc_t;

typedef struct {
    esw_cpu_t *cpu;
    const esw_clock_t *clk;
    const esw_loader_desc_t *d;
    uint8_t loaded, streaming;
    /* streaming state (read from the mailbox at start) */
    uint32_t nbuf, buf_size, buf_base, caps, cmd_seq, next_slot;
    uint32_t last_status;          /* last loader status word (for diagnostics) */
} esw_loader_t;

/* Halt the core, download the blob, verify it. */
esw_err_t esw_loader_load(esw_loader_t *l, esw_cpu_t *cpu, const esw_clock_t *clk, const esw_loader_desc_t *d);

/* Call ABI: run fn(args) until the trap; *ret = ARG0. ESW_ERR_TIMEOUT halts the core before returning. */
esw_err_t esw_loader_call(esw_loader_t *l, uint32_t fn, const uint32_t args[4], uint32_t timeout_ms, uint32_t *ret);
/* Call ABI program: ProgramPage per page_size chunk through page_buf_addr. */
esw_err_t esw_loader_call_program(esw_loader_t *l, uint32_t addr, const uint32_t *data, uint32_t nwords);

/* Streaming ABI. */
esw_err_t esw_loader_stream_start(esw_loader_t *l);
esw_err_t esw_loader_stream_cmd(esw_loader_t *l, esw_ldr_cmd_t cmd, uint32_t addr, uint32_t len, uint32_t arg,
                                uint32_t timeout_ms, uint32_t *result);
/* Ring-buffered program: fills free slots while the loader programs earlier ones. len in words, word-aligned. */
esw_err_t esw_loader_stream_program(esw_loader_t *l, uint32_t addr, const uint32_t *data, uint32_t nwords);
esw_err_t esw_loader_stream_stop(esw_loader_t *l);

/* ---- built-in CRC-32 verify loader (any Cortex-M) ------------------------------------------------------
 * 164-byte ARMv6-M Thumb blob (loaders/crc32/, generated into lib/core/loader/crc32_blob.inc), position
 * independent. Uses ESW_CRC32_LDR_RAM bytes of target scratch RAM: code at +0, 1 KB table at +0x400, stack top at
 * +0x1000. Halts the core and overwrites that RAM. *crc = zlib.crc32(mem[addr:addr+len]). */
#define ESW_CRC32_LDR_RAM 0x1000u
#define ESW_CRC32_CHUNK (256u * 1024u)   /* bytes per call: bounded call time, CRC chained between calls */
esw_err_t esw_loader_crc32(esw_cpu_t *cpu, const esw_clock_t *clk, uint32_t scratch, uint32_t addr, uint32_t len,
                           uint32_t *crc);

#endif

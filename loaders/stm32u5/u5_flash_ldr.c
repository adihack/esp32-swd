/* SPDX-License-Identifier: MIT */
/* esp-swd STM32U5 flash loader, streaming ABI (lib/include/esw/loader.h): runs from SRAM1 while the probe fills the
 * next RAM buffer over SWD, so the SWD transfer overlaps the flash programming instead of adding to it.
 *
 * Position independent: no global data, no calls into libraries, no absolute addresses except the peripherals.
 * Memory layout, all relative to the mailbox the probe passes in R0 (the module's descriptor places it at
 * load + 0x1400):  CRC table at mailbox - 0x400 (1 KB), buffers at mailbox + 0xC00 (NBUF x BUFSIZE).
 *
 * Flash (internal, TrustZone off, non-secure registers): programs 128-byte bursts (BWR) where 8 quad-words in a row
 * hold data, single quad-words elsewhere; erased quad-words (all 0xFF) are skipped and stay erased. A bad status
 * stops the buffer: slot status = FLASH_NSSR error bits (0x10000000 | bits), or LDR_ERR_* below.
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define FLASH_BASE_REG 0x40022000u
#define NSKEYR REG(FLASH_BASE_REG + 0x08u)
#define NSSR   REG(FLASH_BASE_REG + 0x20u)
#define NSCR   REG(FLASH_BASE_REG + 0x28u)
#define OPTR   REG(FLASH_BASE_REG + 0x40u)
#define FLASHSIZE REG(0x0BFA07A0u)

#define SR_EOP     (1u << 0)
#define SR_ERRORS  0x000020FAu   /* OPERR PROGERR WRPERR PGAERR SIZERR PGSERR OPTWERR */
#define SR_BSY     (1u << 16)
#define SR_WDW     (1u << 17)
#define CR_PG      (1u << 0)
#define CR_PER     (1u << 1)
#define CR_MER1    (1u << 2)
#define CR_BKER    (1u << 11)
#define CR_BWR     (1u << 14)
#define CR_MER2    (1u << 15)
#define CR_STRT    (1u << 16)
#define CR_LOCK    (1u << 31)
#define OPTR_SWAP  (1u << 20)
#define OPTR_DUAL  (1u << 21)

#define FLASH_MEM  0x08000000u
#define PAGE       0x2000u

/* streaming ABI, word indexes (byte offsets in loader.h / 4) */
#define MB_MAGIC 0u
#define MB_VERSION 1u
#define MB_CAPS 2u
#define MB_NBUF 3u
#define MB_BUFSIZE 4u
#define MB_BUFBASE 5u
#define MB_CMD_SEQ 6u
#define MB_DONE_SEQ 7u
#define MB_CMD 8u
#define MB_ADDR 9u
#define MB_LEN 10u
#define MB_STATUS 12u
#define MB_RESULT 13u
#define MB_SLOTS 16u
#define LDR_MAGIC 0x4C575345u
#define SLOT_FREE 0u
#define SLOT_FILLED 1u
#define SLOT_BUSY 2u
#define SLOT_DONE 3u
#define CMD_ERASE 1u
#define CMD_CHIP_ERASE 2u
#define CMD_CRC32 3u
#define CMD_EXIT 0xFFu
#define CAPS 0x0Fu   /* erase, chip erase, program, CRC-32 */

#define NBUF 4u
#define BUFSIZE 4096u

#define LDR_ERR_LOCKED 0xE0000001u   /* NSCR stayed locked after the key sequence */
#define LDR_ERR_ALIGN  0xE0000002u   /* address or length not quad-word / page aligned, or outside the flash */
#define LDR_ERR_CMD    0xE0000003u
#define LDR_ERR_FLASH  0x10000000u   /* | FLASH_NSSR error bits */

#define POLY 0xEDB88320u

typedef struct {
    uint32_t size, bank_size, dual, swap;
} geo_t;

/* The trap: first word of the blob, so trap_addr = load_addr. */
__attribute__((naked, used, section(".text.0trap"))) void u5_trap(void)
{
    __asm volatile("bkpt #0x00\n\tb u5_trap");
}

/* Wait until the flash is idle, clear its flags; 0 or LDR_ERR_FLASH | error bits. */
static uint32_t flash_wait(void)
{
    uint32_t sr;
    do {
        sr = NSSR;
    } while (sr & (SR_BSY | SR_WDW));
    if (sr & (SR_ERRORS | SR_EOP))
        NSSR = sr & (SR_ERRORS | SR_EOP);
    return (sr & SR_ERRORS) ? (LDR_ERR_FLASH | (sr & SR_ERRORS)) : 0u;
}

static uint32_t flash_unlock(void)
{
    if (NSCR & CR_LOCK) {
        NSKEYR = 0x45670123u;
        NSKEYR = 0xCDEF89ABu;
        if (NSCR & CR_LOCK)
            return LDR_ERR_LOCKED;
    }
    (void)flash_wait();   /* idle; stale flags of an earlier operation are cleared, not reported */
    return 0;
}

static uint32_t qw_erased(const uint32_t *w)
{
    return (w[0] & w[1] & w[2] & w[3]) == 0xFFFFFFFFu;
}

/* Program len bytes (multiple of 4) from src to dst (16-byte aligned). A short tail is padded with 0xFF. */
static uint32_t program(uint32_t dst, const uint32_t *src, uint32_t len, const geo_t *g)
{
    if ((dst & 15u) || (len & 3u) || dst < FLASH_MEM || len > g->size || dst - FLASH_MEM > g->size - len)
        return LDR_ERR_ALIGN;
    uint32_t st = 0, off = 0;
    while (off < len && !st) {
        const uint32_t a = dst + off, rem = len - off;
        const uint32_t *s = src + off / 4u;
        volatile uint32_t *d = (volatile uint32_t *)a;
        uint32_t burst = (a & 127u) == 0u && rem >= 128u;
        for (uint32_t q = 0; burst && q < 8u; q++)
            burst = !qw_erased(s + 4u * q);
        if (burst) {
            NSCR = CR_PG | CR_BWR;
            for (uint32_t k = 0; k < 32u; k++)
                d[k] = s[k];
            st = flash_wait();
            off += 128u;
            continue;
        }
        uint32_t qw[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        for (uint32_t k = 0; k < 4u && 4u * k < rem; k++)
            qw[k] = s[k];
        if (!qw_erased(qw)) {
            NSCR = CR_PG;
            for (uint32_t k = 0; k < 4u; k++)
                d[k] = qw[k];
            st = flash_wait();
        }
        off += 16u;
    }
    NSCR = 0;
    return st;
}

static uint32_t page_cmd(uint32_t addr, const geo_t *g)
{
    const uint32_t off = addr - FLASH_MEM;
    uint32_t bank2 = g->dual && off >= g->bank_size;
    if (g->dual && g->swap)
        bank2 ^= 1u;
    return CR_PER | (((off % g->bank_size) / PAGE) << 3) | (bank2 ? CR_BKER : 0u);
}

static uint32_t start(uint32_t cr)
{
    NSCR = cr;
    NSCR = cr | CR_STRT;
    uint32_t st = flash_wait();
    NSCR = 0;
    return st;
}

/* Erase [addr, addr+len): whole banks by bank erase, the rest page by page. Page aligned. */
static uint32_t erase(uint32_t addr, uint32_t len, const geo_t *g)
{
    if (((addr - FLASH_MEM) % PAGE) || (len % PAGE) || addr < FLASH_MEM || len > g->size ||
        addr - FLASH_MEM > g->size - len)
        return LDR_ERR_ALIGN;
    uint32_t st = 0;
    while (len && !st) {
        const uint32_t off = addr - FLASH_MEM;
        if (off % g->bank_size == 0u && len >= g->bank_size) {
            uint32_t bank2 = g->dual && off >= g->bank_size;
            if (g->dual && g->swap)
                bank2 ^= 1u;
            st = start(bank2 ? CR_MER2 : CR_MER1);
            addr += g->bank_size;
            len -= g->bank_size;
        } else {
            st = start(page_cmd(addr, g));
            addr += PAGE;
            len -= PAGE;
        }
    }
    return st;
}

static uint32_t crc32(const uint32_t *p, uint32_t len, const uint32_t *tab)
{
    uint32_t crc = 0xFFFFFFFFu;
    const uint32_t *end = p + (len >> 2);
    while (p != end) {
        crc ^= *p++;
        crc = (crc >> 8) ^ tab[crc & 0xFFu];
        crc = (crc >> 8) ^ tab[crc & 0xFFu];
        crc = (crc >> 8) ^ tab[crc & 0xFFu];
        crc = (crc >> 8) ^ tab[crc & 0xFFu];
    }
    const uint8_t *b = (const uint8_t *)p;
    for (len &= 3u; len; len--) {
        crc ^= *b++;
        crc = (crc >> 8) ^ tab[crc & 0xFFu];
    }
    return ~crc;
}

/* Resident loop: u5_stream(mailbox). Returns (into the trap) only on CMD_EXIT. */
__attribute__((used, noinline, section(".text.1stream"))) uint32_t u5_stream(volatile uint32_t *mb)
{
    uint32_t *tab = (uint32_t *)((uintptr_t)mb - 0x400u);
    const uint32_t bufbase = (uint32_t)(uintptr_t)mb + 0xC00u;
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (POLY & (0u - (c & 1u)));
        tab[i] = c;
    }
    geo_t g;
    uint32_t kb = FLASHSIZE & 0xFFFFu;
    if (kb == 0u || kb == 0xFFFFu)
        kb = 2048u;
    const uint32_t optr = OPTR;
    g.size = kb * 1024u;
    g.dual = kb >= 2048u || (optr & OPTR_DUAL);
    g.swap = (optr & OPTR_SWAP) ? 1u : 0u;
    g.bank_size = g.dual ? g.size / 2u : g.size;

    mb[MB_VERSION] = 1u;
    mb[MB_CAPS] = CAPS;
    mb[MB_NBUF] = NBUF;
    mb[MB_BUFSIZE] = BUFSIZE;
    mb[MB_BUFBASE] = bufbase;
    mb[MB_CMD_SEQ] = 0;
    mb[MB_DONE_SEQ] = 0;
    for (uint32_t i = 0; i < NBUF; i++)
        mb[MB_SLOTS + 4u * i] = SLOT_FREE;
    __asm volatile("dmb" ::: "memory");
    mb[MB_MAGIC] = LDR_MAGIC;

    uint32_t next = 0;
    for (;;) {
        volatile uint32_t *slot = mb + MB_SLOTS + 4u * next;
        if (slot[0] == SLOT_FILLED) {
            slot[0] = SLOT_BUSY;
            uint32_t st = flash_unlock();
            if (!st)
                st = program(slot[1], (const uint32_t *)(uintptr_t)(bufbase + next * BUFSIZE), slot[2], &g);
            slot[3] = st;
            __asm volatile("dmb" ::: "memory");
            slot[0] = SLOT_DONE;
            next = (next + 1u) % NBUF;
            continue;
        }
        const uint32_t seq = mb[MB_CMD_SEQ];
        if (seq == mb[MB_DONE_SEQ])
            continue;
        const uint32_t cmd = mb[MB_CMD], a = mb[MB_ADDR], n = mb[MB_LEN];
        uint32_t st = 0, res = 0;
        if (cmd == CMD_EXIT) {
            NSCR = CR_LOCK;
            mb[MB_STATUS] = 0;
            __asm volatile("dmb" ::: "memory");
            mb[MB_DONE_SEQ] = seq;
            return 0;
        } else if (cmd == CMD_ERASE) {
            st = flash_unlock();
            if (!st)
                st = erase(a, n, &g);
        } else if (cmd == CMD_CHIP_ERASE) {
            st = flash_unlock();
            if (!st)
                st = erase(FLASH_MEM, g.size, &g);
        } else if (cmd == CMD_CRC32) {
            res = crc32((const uint32_t *)(uintptr_t)a, n, tab);
        } else {
            st = LDR_ERR_CMD;
        }
        mb[MB_STATUS] = st;
        mb[MB_RESULT] = res;
        __asm volatile("dmb" ::: "memory");
        mb[MB_DONE_SEQ] = seq;
    }
}

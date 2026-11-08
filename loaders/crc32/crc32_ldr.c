/* SPDX-License-Identifier: MIT */
/* esp-swd CRC-32 loader: CRC-32/ISO-HDLC (= zlib.crc32), ARMv6-M Thumb-1 so one blob runs on M0/M0+/M3/M4/M7/M23/M33.
 * Position independent by construction: no global data, no calls, no absolute addresses; the only data reference is
 * the POLY literal, which GCC places in a PC-relative literal pool inside .text.
 *
 * Call ABI (lib/include/esw/loader.h): R0..R3 = args, SP = stack_top, LR = trap|1, PC = fn. R9 unused. Result in R0.
 *   crc32_init(tab)                  -> builds the 256-word table at tab (1 KB, word aligned), returns 0
 *   crc32_update(addr, len, tab, crc)-> zlib.crc32(mem[addr:addr+len], crc); addr word aligned, any len
 */
#include <stdint.h>

#define POLY 0xEDB88320u

/* The trap: first word of the blob, so trap_addr = load_addr. */
__attribute__((naked, used, section(".text.0trap"))) void crc32_trap(void)
{
    __asm volatile("bkpt #0x00\n\tb crc32_trap");
}

__attribute__((used, noinline, section(".text.1init"))) uint32_t crc32_init(uint32_t *tab)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (POLY & (0u - (c & 1u)));
        tab[i] = c;
    }
    return 0;
}

#define STEP(c) ((c) = ((c) >> 8) ^ tab[(c) & 0xFFu])

__attribute__((used, noinline, section(".text.2update")))
uint32_t crc32_update(const uint32_t *p, uint32_t len, const uint32_t *tab, uint32_t crc)
{
    crc = ~crc;
    const uint32_t *end = p + (len >> 2);
    while (p != end) {
        crc ^= *p++;              /* little-endian word = 4 bytes in address order */
        STEP(crc); STEP(crc); STEP(crc); STEP(crc);
    }
    const uint8_t *b = (const uint8_t *)p;
    for (len &= 3u; len; len--) {
        crc ^= *b++;
        STEP(crc);
    }
    return ~crc;
}

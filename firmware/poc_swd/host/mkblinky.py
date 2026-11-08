#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hand-assembled nRF52840 DK test image: unlock APPROTECT, then chase LED1..LED4.

Builds the image, disassembles it (capstone), runs it in an emulator (unicorn) to check the
register writes, and writes device/nrf_blinky.py with the flash words.

    ../../.venv/bin/python host/mkblinky.py      (from firmware/poc_swd)

Image layout (flash 0x00000000):
  0x00  initial SP   0x20040000 (top of 256 KB RAM)
  0x04  reset vector 0x00000009 (code at 0x08, Thumb bit set)
  0x08  code
        APPROTECT->DISABLE = 0x5A   (0x40000558; build code F re-locks at every reset unless firmware
                                     writes this and UICR.APPROTECT = HwDisabled)
        P0->DIRSET = LED1..4, P0->OUTSET = LED1..4 (LEDs P0.13-16, active low: all off)
        loop: OUTCLR = bit (LED on), delay, OUTSET = bit (off), bit <<= 1, wrap after LED4
  ....  literal pool
"""
import struct
import sys
from pathlib import Path

DELAY = 0x00280000           # subs/bne iterations per LED step (~0.2 s at 64 MHz)
APPROTECT_DISABLE = 0x40000558
P0_500 = 0x50000500           # P0 base + 0x500: OUT +4, OUTSET +8, OUTCLR +0xC, DIRSET +0x18
LED_MASK = 0xF << 13          # P0.13..P0.16
SP_INIT = 0x20040000


def assemble(delay=DELAY):
    code = []          # (kind, args) resolved in a second pass
    labels = {}
    lits = [APPROTECT_DISABLE, P0_500, LED_MASK, delay]
    base = 0x08

    def at():
        return base + 2 * len(code)

    def emit(kind, *a):
        code.append((kind, a))

    emit('ldr', 0, 0)                 # r0 = APPROTECT->DISABLE address
    emit('movs', 1, 0x5A)
    emit('str', 1, 0, 0x00)           # APPROTECT->DISABLE = 0x5A
    emit('ldr', 0, 1)                 # r0 = P0 + 0x500
    emit('ldr', 1, 2)                 # r1 = LED mask
    emit('str', 1, 0, 0x18)           # DIRSET
    emit('str', 1, 0, 0x08)           # OUTSET: all LEDs off
    emit('movs', 2, 1)
    emit('lsls', 2, 2, 13)            # r2 = LED1 bit
    labels['loop'] = at()
    emit('str', 2, 0, 0x0C)           # OUTCLR: LED on
    emit('ldr', 3, 3)                 # r3 = delay
    labels['d'] = at()
    emit('subs', 3, 1)
    emit('bne', 'd')
    emit('str', 2, 0, 0x08)           # OUTSET: LED off
    emit('lsls', 2, 2, 1)
    emit('tst', 2, 1)
    emit('bne', 'loop')
    emit('movs', 2, 1)
    emit('lsls', 2, 2, 13)
    emit('b', 'loop')

    end = at()
    pool = (end + 3) & ~3
    halfwords = []
    for i, (kind, a) in enumerate(code):
        pc = base + 2 * i
        if kind == 'ldr':
            rt, li = a
            lit_addr = pool + 4 * li
            off = lit_addr - ((pc + 4) & ~3)
            assert off >= 0 and off % 4 == 0 and off < 1024
            hw = 0x4800 | rt << 8 | off // 4
        elif kind == 'movs':
            hw = 0x2000 | a[0] << 8 | a[1]
        elif kind == 'str':
            rt, rn, off = a
            assert off % 4 == 0 and off < 128
            hw = 0x6000 | (off // 4) << 6 | rn << 3 | rt
        elif kind == 'subs':
            hw = 0x3800 | a[0] << 8 | a[1]
        elif kind == 'lsls':
            rd, rm, imm = a
            hw = imm << 6 | rm << 3 | rd
        elif kind == 'tst':
            rn, rm = a
            hw = 0x4200 | rm << 3 | rn
        elif kind in ('bne', 'b'):
            off = (labels[a[0]] - (pc + 4)) // 2
            hw = (0xD100 | (off & 0xFF)) if kind == 'bne' else (0xE000 | (off & 0x7FF))
        halfwords.append(hw)
    body = b''.join(struct.pack('<H', h) for h in halfwords)
    body += b'\x00' * (pool - end)                       # pad to the pool (0x0000 = movs r0,r0, never run)
    body += b''.join(struct.pack('<I', v) for v in lits)
    image = struct.pack('<II', SP_INIT, base | 1) + body
    image += b'\xff' * (-len(image) % 4)
    return image, base


def disassemble(image, base):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    pool = len(image) - 16
    lines = ['0x%04X: .word 0x%08X  initial SP' % (0, struct.unpack_from('<I', image, 0)[0]),
             '0x%04X: .word 0x%08X  reset vector' % (4, struct.unpack_from('<I', image, 4)[0])]
    for ins in md.disasm(image[base:pool], base):
        lines.append('0x%04X: %-6s %s' % (ins.address, ins.mnemonic, ins.op_str))
    for a in range(pool, len(image), 4):
        lines.append('0x%04X: .word 0x%08X' % (a, struct.unpack_from('<I', image, a)[0]))
    return lines


def emulate(image, steps=200000):
    """Run the image with a tiny delay and record writes to the APPROTECT and GPIO registers."""
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_PC
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    mu.mem_map(0x00000000, 0x1000)
    mu.mem_map(0x20000000, 0x40000)
    mu.mem_map(0x40000000, 0x1000)
    mu.mem_map(0x50000000, 0x1000)
    mu.mem_write(0, image)
    writes = []
    mu.hook_add(UC_HOOK_MEM_WRITE, lambda uc, acc, addr, size, val, ud: writes.append((addr, val)))
    sp, rv = struct.unpack_from('<II', image, 0)
    mu.reg_write(UC_ARM_REG_SP, sp)
    mu.emu_start(rv, 0xFFFFFFFF, count=steps)
    return writes


def main():
    image, base = assemble()
    print('\n'.join(disassemble(image, base)))
    small, _ = assemble(delay=3)
    w = emulate(small)
    names = {0x40000558: 'APPROTECT.DISABLE', 0x50000508: 'P0.OUTSET', 0x5000050C: 'P0.OUTCLR',
             0x50000518: 'P0.DIRSET'}
    print('\nemulated register writes (first 14):')
    for addr, val in w[:14]:
        print('  %-18s <- 0x%08X' % (names.get(addr, hex(addr)), val))
    assert w[0] == (0x40000558, 0x5A), 'APPROTECT.DISABLE must be the first write'
    assert w[1] == (0x50000518, LED_MASK) and w[2] == (0x50000508, LED_MASK)
    on = [v for a, v in w if a == 0x5000050C]
    assert on[:5] == [1 << 13, 1 << 14, 1 << 15, 1 << 16, 1 << 13], on[:5]
    print('emulation checks: OK (unlock first, LEDs as outputs and off, chase 13-14-15-16-13)')

    words = [struct.unpack_from('<I', image, i)[0] for i in range(0, len(image), 4)]
    out = Path(__file__).resolve().parent.parent / 'device' / 'nrf_blinky.py'
    lst = '\n'.join('#   ' + l for l in disassemble(image, base))
    out.write_text('# nrf_blinky.py - GENERATED by host/mkblinky.py, do not edit.\n'
                   '# nRF52840 DK test image: APPROTECT.DISABLE = 0x5A, then LED1..LED4 chase.\n'
                   + lst + '\n'
                   + 'WORDS = [\n' + ''.join('    0x%08X,\n' % v for v in words) + ']\n')
    print('\nwrote %s (%d bytes, %d words)' % (out, len(image), len(words)))


if __name__ == '__main__':
    sys.exit(main())

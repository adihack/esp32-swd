#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rebuild the STM32U5 streaming flash loader and regenerate lib/targets/stm32u5/u5_flash_blob.inc.

    ../../.venv/bin/python build.py            (needs arm-none-eabi-gcc: ARM_GCC_BIN or ~/arm/.../bin)

Steps: compile (Cortex-M33 Thumb-2, position independent), extract the flat binary and the symbol offsets, then run
the blob in the unicorn emulator against a model of the STM32U5 FLASH registers, playing the probe's side of the
streaming ABI: page, bank and chip erase (with and without SWAP_BANK), programming through the buffer ring (bursts,
single quad-words, skipped erased quad-words, a padded tail), an out-of-range request, CRC-32 against zlib, EXIT.
The library build never needs arm-gcc: it only includes the generated, checked-in .inc.
"""
import glob
import os
import random
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "lib", "targets", "stm32u5", "u5_flash_blob.inc")
BIN = os.environ.get("ARM_GCC_BIN") or (glob.glob(os.path.expanduser("~/arm/arm-gnu-toolchain-*/bin")) or [""])[0]
CFLAGS = ["-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "-O2", "-fpie", "-ffreestanding", "-nostdlib",
          "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-ffunction-sections", "-fno-jump-tables",
          "-Wall", "-Wextra", "-Wconversion", "-Werror"]

# RAM layout (offsets from the load address), shared with lib/targets/stm32u5/stm32u5.c through the .inc
CODE_MAX, TAB_OFF, MB_OFF, STACK_TOP, BUF_OFF, NBUF, BUFSIZE = 0x1000, 0x1000, 0x1400, 0x2000, 0x2000, 4, 4096
assert MB_OFF - 0x400 == TAB_OFF and MB_OFF + 0xC00 == BUF_OFF   # what the loader derives from the mailbox

FLASH, FREG = 0x08000000, 0x40022000
KEY1, KEY2 = 0x45670123, 0xCDEF89AB


def tool(name):
    return os.path.join(BIN, "arm-none-eabi-" + name)


def build(tmp):
    src, ld = os.path.join(HERE, "u5_flash_ldr.c"), os.path.join(HERE, "u5_flash_ldr.ld")
    obj, elf, binf = (os.path.join(tmp, "u5." + x) for x in ("o", "elf", "bin"))
    subprocess.run([tool("gcc")] + CFLAGS + ["-c", src, "-o", obj], check=True)
    subprocess.run([tool("gcc"), "-mcpu=cortex-m33", "-mthumb", "-nostdlib", "-Wl,-T," + ld,
                    "-Wl,--no-warn-rwx-segments", obj, "-o", elf], check=True)
    subprocess.run([tool("objcopy"), "-O", "binary", elf, binf], check=True)
    relocs = subprocess.run([tool("readelf"), "-r", elf], check=True, capture_output=True, text=True).stdout
    if "Relocation section" in relocs:
        sys.exit("loader has relocations: not position independent")
    syms = {}
    for line in subprocess.run([tool("nm"), elf], check=True, capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("u5_trap", "u5_stream"):
            syms[parts[2]] = int(parts[0], 16)
    blob = open(binf, "rb").read()
    blob += b"\0" * (-len(blob) % 4)
    version = subprocess.run([tool("gcc"), "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    return blob, syms, version


class U5:
    """unicorn + a model of the STM32U5 FLASH registers (2 MB, dual bank, 8 KB pages)."""

    def __init__(self, blob, syms, base, swap=False):
        from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE
        self.mu = mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.base, self.syms = base, syms
        mu.mem_map(FLASH, 2 << 20)
        mu.mem_map(0x0BFA0000, 0x1000)
        mu.mem_write(0x0BFA07A0, struct.pack("<I", 0xFFFF0800))   # 2048 KB
        mu.mem_map(0x20000000, 0x40000)
        mu.mem_write(base, blob)
        self.nscr, self.nssr, self.keys = 1 << 31, 0, 0
        self.optr = 0x1FEFF8AA | ((1 << 20) if swap else 0)
        self.swap = swap
        self.erases, self.writes, self.bad_writes = [], set(), []
        mu.mmio_map(FREG, 0x1000, self._rd, None, self._wr, None)
        mu.hook_add(UC_HOOK_MEM_WRITE, self._flash_write, begin=FLASH, end=FLASH + (2 << 20) - 1)
        self.mb = base + MB_OFF
        self.pc = None

    # FLASH registers
    def _rd(self, uc, off, size, data):
        return {0x20: self.nssr, 0x28: self.nscr, 0x40: self.optr}.get(off, 0)

    def _wr(self, uc, off, size, value, data):
        if off == 0x08:   # NSKEYR
            if self.keys == 0 and value == KEY1:
                self.keys = 1
            elif self.keys == 1 and value == KEY2:
                self.keys, self.nscr = 0, self.nscr & ~(1 << 31)
            else:
                self.keys = 2   # wrong sequence: locked until reset
        elif off == 0x20:
            self.nssr &= ~value
        elif off == 0x28:
            if self.nscr & (1 << 31):
                return   # locked: writes ignored
            self.nscr = value
            if value & (1 << 16):
                self._start(value)

    def _bank_addr(self, physical):
        return FLASH + ((physical ^ (1 if self.swap else 0)) << 20)

    def _start(self, cr):
        if cr & (1 << 1):   # PER
            pnb, phys = (cr >> 3) & 0xFF, (cr >> 11) & 1
            a = self._bank_addr(phys) + pnb * 0x2000
            self.mu.mem_write(a, b"\xff" * 0x2000)
            self.erases.append(("page", a))
        for bit, phys in ((1 << 2, 0), (1 << 15, 1)):   # MER1, MER2
            if cr & bit:
                a = self._bank_addr(phys)
                self.mu.mem_write(a, b"\xff" * (1 << 20))
                self.erases.append(("bank", a))

    def _flash_write(self, uc, access, addr, size, value, data):
        if not (self.nscr & 1) or size != 4 or addr & 3:
            self.bad_writes.append((hex(addr), size, hex(self.nscr)))
        self.writes.add(addr & ~15)

    # probe side
    def run(self, count=2_000_000):
        from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
        trap = self.base + (self.syms["u5_trap"] & ~1)
        if self.pc is None:
            self.mu.reg_write(UC_ARM_REG_R0, self.mb)
            self.mu.reg_write(UC_ARM_REG_SP, self.base + STACK_TOP)
            self.mu.reg_write(UC_ARM_REG_LR, trap | 1)
            self.pc = self.base + (self.syms["u5_stream"] & ~1)
        self.mu.emu_start(self.pc | 1, trap, count=count)
        self.pc = self.mu.reg_read(UC_ARM_REG_PC)
        return self.pc == trap

    def w(self, off, v):
        self.mu.mem_write(self.mb + off, struct.pack("<I", v))

    def r(self, off):
        return struct.unpack("<I", self.mu.mem_read(self.mb + off, 4))[0]

    def cmd(self, c, a=0, n=0):
        seq = self.r(0x1C) + 1
        for off, v in ((0x20, c), (0x24, a), (0x28, n)):
            self.w(off, v)
        self.w(0x18, seq)
        done = self.run()
        assert self.r(0x1C) == seq, "command %d not completed" % c
        return self.r(0x30), self.r(0x34), done

    def program(self, addr, data):
        """Through the ring like esw_loader_stream_program: fill slots in order, let the loader run."""
        nxt, off = getattr(self, "nxt", 0), 0
        statuses = []
        while off < len(data):
            n = min(BUFSIZE, len(data) - off)
            slot = 0x40 + 16 * nxt
            st = self.r(slot)
            if st == 3:
                statuses.append(self.r(slot + 12))
            self.mu.mem_write(self.base + BUF_OFF + nxt * BUFSIZE, data[off:off + n])
            self.w(slot + 4, addr + off)
            self.w(slot + 8, n)
            self.w(slot, 1)
            self.run()
            assert self.r(slot) == 3, "slot not done"
            statuses.append(self.r(slot + 12))
            self.w(slot, 0)
            nxt, off = (nxt + 1) % NBUF, off + n
        self.nxt = nxt
        return statuses


def check(blob, syms):
    rnd = random.Random(5)
    for base in (0x20000000, 0x20010000):
        for swap in (False, True):
            u = U5(blob, syms, base, swap)
            u.mu.mem_write(FLASH, b"\x00" * (2 << 20))
            assert not u.run(), "loader exited at start"
            assert u.r(0) == 0x4C575345 and u.r(4) == 1 and u.r(0x0C) == NBUF and u.r(0x10) == BUFSIZE
            assert u.r(0x14) == base + BUF_OFF, "buffer base"
            # page erase across the bank border, then a whole bank
            st, _, _ = u.cmd(1, FLASH + 0x100000 - 0x2000, 0x4000)
            assert st == 0
            assert u.erases == [("page", FLASH + 0x0FE000), ("page", FLASH + 0x100000)], u.erases
            assert u.mu.mem_read(FLASH + 0x0FC000, 0x2000) == b"\0" * 0x2000
            assert u.mu.mem_read(FLASH + 0x0FE000, 0x4000) == b"\xff" * 0x4000
            assert u.mu.mem_read(FLASH + 0x102000, 16) == b"\0" * 16
            u.erases.clear()
            st, _, _ = u.cmd(1, FLASH + 0x100000, 0x100000)
            assert st == 0 and u.erases == [("bank", FLASH + 0x100000)], u.erases
            assert u.mu.mem_read(FLASH + 0x0FC000, 4) == b"\0" * 4
            # misaligned erase is refused, nothing erased
            u.erases.clear()
            st, _, _ = u.cmd(1, FLASH + 0x1000, 0x2000)
            assert st == 0xE0000002 and not u.erases
            # chip erase
            st, _, _ = u.cmd(2)
            assert st == 0 and sorted(u.erases[-2:]) == sorted([("bank", FLASH), ("bank", FLASH + 0x100000)])
            # program: 16-byte aligned start (not 128), erased quad-words in the middle, a tail of 12 bytes
            addr = FLASH + 0x20030
            img = bytearray(rnd.getrandbits(8) for _ in range(3 * BUFSIZE + 300))
            img[0x500:0x540] = b"\xff" * 0x40        # 4 erased quad-words: skipped
            img[0x1000:0x1080] = b"\xff" * 0x80      # a whole erased burst: skipped
            img = bytes(img[:len(img) - (len(img) % 4)])
            u.writes.clear()
            sts = u.program(addr, img)
            assert all(s == 0 for s in sts), sts
            got = bytes(u.mu.mem_read(addr, len(img)))
            assert got == img, "programmed data differs"
            tail = (-len(img)) % 16
            assert bytes(u.mu.mem_read(addr + len(img), tail)) == b"\xff" * tail
            skipped = {a for a in range(addr, addr + len(img), 16) if img[a - addr:a - addr + 16] == b"\xff" * 16}
            assert skipped and not (skipped & u.writes), "erased quad-words were written"
            assert not u.bad_writes, u.bad_writes[:3]
            # out of range: refused
            sts = u.program(FLASH + (2 << 20) - 16, b"\x11" * 32)
            assert sts == [0xE0000002], sts
            # CRC over flash
            st, crc, _ = u.cmd(3, addr, len(img))
            assert st == 0 and crc == zlib.crc32(img), (hex(crc), hex(zlib.crc32(img)))
            st, crc, _ = u.cmd(3, addr, 7)
            assert crc == zlib.crc32(img[:7])
            # exit into the trap, flash locked again
            st, _, done = u.cmd(0xFF)
            assert done and u.nscr & (1 << 31)


def main():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        blob, syms, version = build(tmp)
    for need in ("u5_trap", "u5_stream"):
        if need not in syms:
            sys.exit("missing symbol " + need)
    if len(blob) > CODE_MAX:
        sys.exit("blob is %d bytes, larger than the %d-byte code slot" % (len(blob), CODE_MAX))
    check(blob, syms)
    words = struct.unpack("<%dI" % (len(blob) // 4), blob)
    with open(OUT, "w") as f:
        f.write("/* SPDX-License-Identifier: MIT */\n")
        f.write("/* GENERATED by loaders/stm32u5/build.py from loaders/stm32u5/u5_flash_ldr.c - do not edit.\n")
        f.write(" * %s; verified in unicorn against a model of the STM32U5 FLASH registers. */\n" % version)
        f.write("#define ESW_U5LDR_WORDS %du\n" % len(words))
        f.write("#define ESW_U5LDR_OFF_TRAP 0x%02Xu\n" % syms["u5_trap"])
        f.write("#define ESW_U5LDR_OFF_STREAM 0x%02Xu   /* even offsets: Thumb state comes from xPSR.T */\n"
                % (syms["u5_stream"] & ~1))
        for name, v in (("TAB", TAB_OFF), ("MAILBOX", MB_OFF), ("STACK_TOP", STACK_TOP), ("BUF", BUF_OFF),
                        ("RAM", BUF_OFF + NBUF * BUFSIZE)):
            f.write("#define ESW_U5LDR_%s 0x%04Xu\n" % (name, v))
        f.write("static const uint32_t esw_u5_ldr_blob[ESW_U5LDR_WORDS] = {\n")
        for i in range(0, len(words), 6):
            f.write("    " + ", ".join("0x%08Xu" % w for w in words[i:i + 6]) + ",\n")
        f.write("};\n")
    print("wrote %s: %d bytes, offsets %s, verified" % (os.path.relpath(OUT), len(blob),
          {k: hex(v) for k, v in syms.items()}))


if __name__ == "__main__":
    main()

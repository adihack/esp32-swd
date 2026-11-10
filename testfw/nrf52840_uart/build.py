#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the nRF52840 UART test firmware (build/uart_test.bin, flashed at 0 by host/python/examples/uart_test.py).

    ../../.venv/bin/python build.py            (needs arm-none-eabi-gcc: ARM_GCC_BIN or ~/arm/.../bin)
"""
import glob
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")
BIN = os.environ.get("ARM_GCC_BIN") or (glob.glob(os.path.expanduser("~/arm/arm-gnu-toolchain-*/bin")) or [""])[0]
CFLAGS = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-Os", "-ffreestanding", "-fno-builtin",
          "-fno-tree-loop-distribute-patterns", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
          "-Wconversion", "-Werror", "-g"]


def tool(name):
    return os.path.join(BIN, "arm-none-eabi-" + name)


def build():
    os.makedirs(OUT, exist_ok=True)
    elf, binf = os.path.join(OUT, "uart_test.elf"), os.path.join(OUT, "uart_test.bin")
    subprocess.run([tool("gcc")] + CFLAGS + ["-nostdlib", "-Wl,-T," + os.path.join(HERE, "link.ld"),
                    "-Wl,--gc-sections", "-Wl,--no-warn-rwx-segments", os.path.join(HERE, "main.c"), "-lgcc",
                    "-o", elf], check=True)
    subprocess.run([tool("objcopy"), "-O", "binary", elf, binf], check=True)
    img = open(binf, "rb").read()
    sp, reset = struct.unpack_from("<II", img)
    if sp != 0x20040000 or not reset & 1 or reset >= len(img):
        sys.exit("bad vector table: SP 0x%08X, reset 0x%08X" % (sp, reset))
    print("built %s: %d bytes, reset handler 0x%X" % (os.path.relpath(binf), len(img), reset & ~1))
    return binf


if __name__ == "__main__":
    build()

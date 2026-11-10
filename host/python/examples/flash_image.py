#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd example: flash a .bin or Intel .hex file into a target and verify it.

    ../../../.venv/bin/python flash_image.py IMAGE [--channel A|B] [--core N] [--addr 0x08000000] [--allow-erase]
    ../../../.venv/bin/python flash_image.py --info [--channel B]          # identify only, write nothing

Steps: hold the SWD group (interlock: VTref first) -> attach (the firmware's chip modules identify the part and its
memory map) -> unlock (only with --allow-erase: a locked part is mass-erased) -> per run of adjacent pages: erase,
program, verify by on-target CRC -> detach (pin reset: the new image starts) -> release (isolated again).

Works with every chip module in the firmware: nRF52, nRF5340 (--core 0 application, 1 network), STM32U5 (internal
flash, TrustZone off). A .bin goes to --addr (default: start of the first flash region). Data in a .hex is grouped
by erase page; gaps inside a touched page are filled with 0xFF, so two records in one page never erase each other.

nRF5340: only flash images that run Nordic's startup code (SystemInit). After every reset it reopens the debug
port; without it the port stays shut after the next reset, and only an ERASEALL (which also wipes UICR and its KMU
key slots) brings it back.
"""
import argparse
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, EswError, proto as P  # noqa: E402


def read_hex(path):
    """Intel HEX -> {address: byte}. Record types 00 data, 01 EOF, 02/04 address extension; 03/05 ignored."""
    mem, base = {}, 0
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError("%s:%d: not an Intel HEX record" % (path, n))
        rec = bytes.fromhex(line[1:])
        if len(rec) < 5 or len(rec) != rec[0] + 5 or sum(rec) & 0xFF:
            raise ValueError("%s:%d: bad length or checksum" % (path, n))
        cnt, addr, typ, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:-1]
        if typ == 0:
            for i, b in enumerate(data):
                mem[base + addr + i] = b
        elif typ == 1:
            break
        elif typ == 2:
            base = int.from_bytes(data, "big") << 4
        elif typ == 4:
            base = int.from_bytes(data, "big") << 16
    return mem


def page_runs(mem, regions):
    """{address: byte} -> [(start, bytes)] covering whole erase pages, adjacent pages merged, gaps 0xFF."""
    pages = {}
    for a in mem:
        r = next((r for r in regions if r["start"] <= a < r["start"] + r["size"]), None)
        if r is None:
            raise ValueError("address 0x%08X is outside every memory region of the target" % a)
        if r["kind"] not in ("flash", "otp"):
            raise ValueError("address 0x%08X is in %s, not flash" % (a, r["kind"]))
        pg = r["sector"]
        pages[a - (a - r["start"]) % pg] = pg
    runs, cur = [], None
    for start in sorted(pages):
        if cur and cur[0] + cur[1] == start:
            cur[1] += pages[start]
        else:
            cur = [start, pages[start]]
            runs.append(cur)
    out = []
    for start, size in runs:
        buf = bytearray(b"\xff" * size)
        for a in range(start, start + size):
            if a in mem:
                buf[a - start] = mem[a]
        out.append((start, bytes(buf)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--channel", default="A", choices=("A", "B"))
    ap.add_argument("--core", type=int, default=0, help="multi-core parts: nRF5340 0 application, 1 network")
    ap.add_argument("--addr", type=lambda s: int(s, 0), help=".bin load address (default: start of flash)")
    ap.add_argument("--clock", type=int, default=8000000, help="SWCLK in Hz")
    ap.add_argument("--allow-erase", action="store_true", help="unlock a protected part by mass erase")
    ap.add_argument("--info", action="store_true", help="identify the target and print its memory map only")
    args = ap.parse_args()
    if not args.info and not args.image:
        ap.error("an IMAGE is needed (or --info)")
    ch = "AB".index(args.channel)

    with Esw(args.port, timeout=120) as d:
        d.window = 4   # capped by the board: 4 per channel on proto >= 4, else 3
        d.hold(ch, P.GROUP_SWD)
        st = d.wait_group(ch, P.GROUP_SWD, "CLOSED", 2.0)
        if st["swd"] != "CLOSED":
            d.release(ch, P.GROUP_SWD)
            sys.exit("channel %s: SWD group %s (VTref %s mV): DUT unpowered, or the cable is rotated (pin 1 on the "
                     "DUT's nRESET reads about 2.7 V)" % (args.channel, st["swd"], st["vtref_mv"]))
        try:
            info = d.attach(ch, args.clock, 1, core=args.core)
            print("%s on channel %s (DPIDR 0x%08X, %d kHz, VTref %d mV)%s" % (
                info["part"], args.channel, info["dpidr"], info["khz"], st["vtref_mv"],
                ", LOCKED" if info["locked"] else ""))
            for r in info["regions"]:
                print("  %-5s 0x%08X  %8d bytes, %d-byte pages" % (r["kind"], r["start"], r["size"], r["sector"]))
            if args.info:
                return 0
            if info["locked"]:
                if not args.allow_erase:
                    sys.exit("target is protected: --allow-erase mass-erases it to regain access")
                info = d.unlock(ch, allow_erase=True)   # no re-attach: that would reset the part
                print("unlocked:", info["part"])
            regions = info["regions"]
            if args.image.lower().endswith((".hex", ".ihex")):
                mem = read_hex(args.image)
            else:
                data = open(args.image, "rb").read()
                base = args.addr if args.addr is not None else next(r["start"] for r in regions
                                                                     if r["kind"] == "flash")
                mem = {base + i: b for i, b in enumerate(data)}
            if not mem:
                sys.exit("image is empty")
            runs = page_runs(mem, regions)
            total, t0 = sum(len(b) for _, b in runs), time.monotonic()
            for start, buf in runs:
                t1 = time.monotonic()
                d.flash(ch, start, buf)   # erase the run's pages, program, on-target CRC verify
                print("  0x%08X  %7d bytes  %.2f s  crc32 %08X ok" % (start, len(buf), time.monotonic() - t1,
                                                                       zlib.crc32(buf)))
            dt = time.monotonic() - t0
            print("%d bytes (%d data) in %.2f s, %.0f KB/s, verified" % (total, len(mem), dt, total / dt / 1024))
        except EswError as e:
            print("error:", e)
            return 1
        finally:
            d.detach(ch)   # debug off, pin reset: the image starts
            d.release(ch, P.GROUP_SWD)
    return 0


if __name__ == "__main__":
    sys.exit(main())

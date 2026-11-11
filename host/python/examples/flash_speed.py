#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd flash speed test: erase, program and verify random data on an nRF52840 (channel A by default),
then restore the LED-chase test image so the DK keeps running something sane.

    ../../../.venv/bin/python flash_speed.py [--size 1M|64K|...] [--window N] [--channel A]
"""
import argparse
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, proto as P  # noqa: E402
from nrf52_blinky import BLINKY  # noqa: E402


def size_arg(s):
    s = s.upper()
    mul = 1024 * 1024 if s.endswith("M") else 1024 if s.endswith("K") else 1
    return int(s.rstrip("MK")) * mul


def crc_check(d, ch, data):
    got, want = d.crc32(ch, 0, len(data)), zlib.crc32(data)
    if got != want:
        raise RuntimeError("CRC mismatch: target 0x%08X, image 0x%08X" % (got, want))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="device, board id or unix:<socket>; default: the only board (through esw serve if it runs)")
    ap.add_argument("--channel", default="A", choices="AB")
    ap.add_argument("--size", type=size_arg, default=64 * 1024)
    ap.add_argument("--window", type=int, default=1, help="frames in flight (1 = strict request/response)")
    ap.add_argument("--phy", type=int, default=1, choices=(0, 1), help="1 dedicated GPIO (default), 0 bit-bang")
    ap.add_argument("--clock", type=float, default=0, help="dgpio: SWCLK in Hz (0 = 8 MHz)")
    ap.add_argument("--verify", default="readback", choices=("readback", "crc"),
                    help="readback: send the image again and compare on the board; crc: CRC computed on the target")
    args = ap.parse_args()
    ch = "AB".index(args.channel)
    data = os.urandom(args.size)
    d = Esw(args.port, timeout=120)
    d.window = args.window

    d.hold(ch, P.GROUP_SWD)
    st = d.wait_group(ch, P.GROUP_SWD, "CLOSED", 2.0)
    t = d.attach(ch, int(args.clock), args.phy)
    print("%s, VTref %s mV, %d KB random data, window %d, phy %d%s, verify %s" % (t["part"], st["vtref_mv"],
          args.size // 1024, args.window, args.phy, (" @ %d kHz" % t["khz"]) if args.phy else "", args.verify))
    res = {}
    uicr_before = d.mem_read(ch, 0x10001200, 3)
    try:
        for name, fn in (("erase", lambda: d.erase(ch, 0, args.size)),
                         ("program", lambda: d.program(ch, 0, data)),
                         ("verify", (lambda: d.verify(ch, 0, data)) if args.verify == "readback" else
                          (lambda: crc_check(d, ch, data)))):
            t0 = time.monotonic()
            fn()
            res[name] = time.monotonic() - t0
            print("  %-8s %7.2f s  %7.1f KB/s" % (name, res[name], args.size / 1024 / res[name]))
        total = sum(res.values())
        print("  %-8s %7.2f s  (%.1f KB/s for erase+program+verify)" % ("total", total, args.size / 1024 / total))
        uicr_after = d.mem_read(ch, 0x10001200, 3)
        print("  UICR PSELRESET/APPROTECT before %s after %s: %s" % (
            " ".join("%08X" % v for v in uicr_before), " ".join("%08X" % v for v in uicr_after),
            "preserved" if uicr_before == uicr_after else "CHANGED"))
        s = d.stats(ch)
        print("  SWD packets %d, WAIT %d, FAULT %d, no-ACK %d, parity %d" %
              (s["xfers"], s["wait"], s["fault"], s["noack"], s["parity"]))
    finally:
        # restore the LED-chase image (page 0) so the DK runs it again after the reset
        image = struct.pack("<16I", *BLINKY)
        d.erase(ch, 0, 4096)
        d.program(ch, 0, image)
        d.verify(ch, 0, image)
        d.reset(ch)
        d.detach(ch)
        d.release(ch, P.GROUP_SWD)
        print("  restored the LED-chase image and released the channel")


if __name__ == "__main__":
    main()

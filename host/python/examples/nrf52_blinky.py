#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd example: flash the 64-byte LED-chase test image into an nRF52840 DK and prove it runs.

    ../../../.venv/bin/python nrf52_blinky.py [--port /dev/ttyACM0] [--channel A] [--allow-erase] [--half-loops N]

Steps: device info -> hold the SWD group (firmware interlock: VTref first) -> attach (connect, detect module)
-> unlock (only with --allow-erase) -> RAM throughput benchmark -> erase/program/verify -> UICR (reset pin,
APPROTECT HwDisabled) -> reset -> watch the LED bits in P0.OUT -> detach -> release (isolated again).
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, EswError, proto as P  # noqa: E402

# host/mkblinky.py output (firmware/poc_swd/device/nrf_blinky.py): APPROTECT->DISABLE = 0x5A, LED1..4 chase
BLINKY = [0x20040000, 0x00000009, 0x215A4809, 0x48096001, 0x61814909, 0x22016081, 0x60C20352, 0x3B014B07,
          0x6082D1FD, 0x420A0052, 0x2201D1F7, 0xE7F40352, 0x40000558, 0x50000500, 0x0001E000, 0x00280000]
UICR_PSELRESET0, UICR_PSELRESET1, UICR_APPROTECT = 0x10001200, 0x10001204, 0x10001208
P0_OUT = 0x50000504
LEDS = ((13, "LED1"), (14, "LED2"), (15, "LED3"), (16, "LED4"))


def step(msg):
    print("\n== " + msg)


def timed(fn, *a):
    t0 = time.monotonic()
    r = fn(*a)
    return r, (time.monotonic() - t0) * 1000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="device, board id or unix:<socket>; default: the only board (through esw serve if it runs)")
    ap.add_argument("--channel", default="A", choices="AB")
    ap.add_argument("--allow-erase", action="store_true", help="permit a mass erase if the chip is locked")
    ap.add_argument("--phy", type=int, default=1, choices=(0, 1), help="1 dedicated GPIO (default), 0 bit-bang")
    ap.add_argument("--clock", type=float, default=0,
                    help="phy 1: SWCLK in Hz (0 = 8 MHz); phy 0: half-period busy loops (0 = fastest)")
    args = ap.parse_args()
    ch = "AB".index(args.channel)
    dev = Esw(args.port)

    step("device")
    info = dev.info()
    print("  %s, protocol %d, uptime %.1f s, reset reason %d, free heap %d"
          % (info["fw"], info["proto"], info["uptime_ms"] / 1000, info["reset_reason"], info["free_heap"]))
    print("  channel %s before: %s" % (args.channel, dev.state(ch)))

    step("isolation: hold the SWD group (firmware closes VTref, checks it, then closes SWD)")
    dev.hold(ch, P.GROUP_SWD)
    st = dev.wait_group(ch, P.GROUP_SWD, "CLOSED", timeout=2.0)
    print("  %s" % st)
    if st["swd"] != "CLOSED":
        print("  SWD group not closed: DUT not powered or VTref outside 2.8-3.6 V. Releasing.")
        dev.release(ch, P.GROUP_SWD)
        return 1

    try:
        step("attach: connect, power-up, detect target module")
        try:
            t, ms = timed(dev.attach, ch, int(args.clock), args.phy)
        except EswError as e:
            print("  attach failed: %s" % e)
            return 1
        print("  DPIDR 0x%08X, part %s, locked=%s (%.0f ms)" % (t["dpidr"], t["part"], t["locked"], ms))
        for r in t["regions"]:
            print("    %-5s 0x%08X + 0x%X (sector %d)" % (r["kind"], r["start"], r["size"], r["sector"]))

        if t["locked"]:
            step("unlock")
            if not args.allow_erase:
                print("  APPROTECT is on. Rerun with --allow-erase to mass-erase the chip (DESTRUCTIVE).")
                return 1
            _, ms = timed(dev.unlock, ch, True)
            print("  ERASEALL + UICR.APPROTECT=HwDisabled done (%.0f ms), written in one session: a reset before the"
                  " UICR write would re-engage APPROTECT" % ms)

        step("RAM throughput (MEM-AP block transfers, 16 KB at 0x20008000)")
        blob = os.urandom(16384)
        _, wms = timed(dev.mem_write, ch, 0x20008000, blob)
        back, rms = timed(dev.mem_read, ch, 0x20008000, 4096)
        ok = struct.pack("<4096I", *back) == blob
        print("  write %.0f ms (%.1f KB/s), read %.0f ms (%.1f KB/s), compare %s"
              % (wms, 16 / wms * 1000, rms, 16 / rms * 1000, "OK" if ok else "MISMATCH"))
        print("  device-side last op: %d us; stats %s" % (dev.stats(ch)["last_op_us"], dev.stats(ch)))

        step("flash: erase page 0, program, verify")
        image = struct.pack("<16I", *BLINKY)
        _, ms = timed(dev.erase, ch, 0, 4096)
        print("  erase   %.0f ms" % ms)
        _, ms = timed(dev.program, ch, 0, image)
        print("  program %.0f ms (64 bytes)" % ms)
        _, ms = timed(dev.verify, ch, 0, image)
        print("  verify  %.0f ms: OK" % ms)

        step("UICR: reset pin P0.18 and APPROTECT HwDisabled (only if still erased)")
        uicr = dev.mem_read(ch, UICR_PSELRESET0, 3)
        print("  before: PSELRESET 0x%08X 0x%08X, APPROTECT 0x%08X" % tuple(uicr))
        for addr, val, cur in ((UICR_PSELRESET0, 0x12, uicr[0]), (UICR_PSELRESET1, 0x12, uicr[1]),
                               (UICR_APPROTECT, 0xFFFFFF5A, uicr[2])):
            if cur == 0xFFFFFFFF:
                dev.program(ch, addr, struct.pack("<I", val))
        print("  after:  PSELRESET 0x%08X 0x%08X, APPROTECT 0x%08X" % tuple(dev.mem_read(ch, UICR_PSELRESET0, 3)))

        step("reset (CTRL-AP): the image must run and keep APPROTECT open")
        dev.reset(ch)
        seen = []
        for _ in range(10):
            out = dev.mem_read(ch, P0_OUT, 1)[0]
            seen.append(",".join(n for b, n in LEDS if not (out >> b) & 1) or "-")
            time.sleep(0.12)
        print("  LEDs on, every 120 ms: " + " ".join(seen))
        print("  stats %s" % dev.stats(ch))
    finally:
        step("detach and release (isolated again)")
        try:
            dev.detach(ch)
        except Exception as e:  # noqa: BLE001
            print("  detach: %s" % e)
        dev.release(ch, P.GROUP_SWD)
        print("  %s" % dev.wait_group(ch, P.GROUP_SWD, "OPEN", timeout=1.0))
        if dev.bad_frames:
            print("  (dropped %d malformed frames)" % dev.bad_frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())

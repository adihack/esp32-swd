#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Device-side SWD PHY benchmark over a range of clock settings (no USB in the timed loop).

    ../../../.venv/bin/python swd_bench.py [--channel A] [--clocks 0,2,5,10,20]
The clock argument is passed to SWD_ATTACH (bit-bang PHY: half-period busy loops; future PHYs: see firmware).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, proto as P  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="/dev/ttyACM0")
ap.add_argument("--channel", default="A", choices="AB")
ap.add_argument("--clocks", default="0", help="bit-bang: half-period loops; dgpio: Hz (e.g. 1e6,8e6)")
ap.add_argument("--phy", type=int, default=0, help="0 bit-bang, 1 dedicated GPIO")
ap.add_argument("--words", type=int, default=65536)
args = ap.parse_args()
ch = "AB".index(args.channel)
d = Esw(args.port, timeout=60)
d.hold(ch, P.GROUP_SWD)
d.wait_group(ch, P.GROUP_SWD, "CLOSED", 2.0)
print("%-8s %-22s %10s %9s %8s %6s %s" % ("clock", "test", "time", "rate", "us/pkt", "errs", "wait/fault/noack/parity"))
try:
    for clk in [int(float(x)) for x in args.clocks.split(",")]:
        try:
            t = d.attach(ch, clk, args.phy)
        except Exception as e:  # noqa: BLE001
            print("%-8d attach failed: %s" % (clk, e))
            continue
        if args.phy:
            print("%-8d attached, actual SWCLK %d kHz" % (clk, t["khz"]))
        for mode, name, n in ((0, "DPIDR reads", 20000), (1, "RAM block write", args.words), (2, "RAM block read+check", args.words)):
            r = d.bench(ch, mode, n)
            rate = ("%6.1f KB/s" % (n * 4 / 1024 / (r["us"] / 1e6))) if mode and r["us"] else ("%7.0f/s" % (n / (r["us"] / 1e6)) if r["us"] else "-")
            print("%-8d %-22s %8.1f ms %10s %8.2f %6d %d/%d/%d/%d%s" % (
                clk, name, r["us"] / 1000, rate, r["us"] / max(1, r["packets"]), r["errors"], r["wait"], r["fault"],
                r["noack"], r["parity"], "" if r["status"] == 0 else "  STATUS %d" % r["status"]))
        d.detach(ch)
finally:
    d.release(ch, P.GROUP_SWD)

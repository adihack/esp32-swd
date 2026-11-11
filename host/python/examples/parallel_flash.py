#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd benchmark: flash both channels one after the other, then both at once, and compare.

    ../../../.venv/bin/python parallel_flash.py --a IMAGE [--a-core N] [--a-addr X] --b IMAGE [--b-core N] [--b-addr X]
                                                [--mode both|seq|par] [--rounds 1]

Each channel: hold the SWD group -> attach -> per run of adjacent pages: erase, program, on-target CRC verify ->
detach (pin reset: the image starts) -> release. Images: .bin (at --x-addr, default start of flash) or Intel .hex.
"At once" means two host threads on the same USB link; whether the device really works on both channels at the
same time is what this measures (wall time near the slower channel: parallel; near the sum: serialised).
"""
import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, proto as P  # noqa: E402
from esw import image  # noqa: E402


def load(path, addr, regions):
    return image.page_runs(image.load(path, regions, addr), regions)


def flash_channel(d, ch, core, runs, clock, out):
    t0 = time.monotonic()
    d.hold(ch, P.GROUP_SWD)
    if d.wait_group(ch, P.GROUP_SWD, "CLOSED", 2.0)["swd"] != "CLOSED":
        raise RuntimeError("channel %s: SWD group not closed" % "AB"[ch])
    try:
        d.attach(ch, clock, 1, core=core)
        for start, buf in runs:
            d.flash(ch, start, buf)
    finally:
        d.detach(ch)
        d.release(ch, P.GROUP_SWD)
    out["AB"[ch]] = time.monotonic() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="device, board id or unix:<socket>; default: the only board (through esw serve if it runs)")
    for c in "ab":
        ap.add_argument("--" + c, required=True, metavar="IMAGE")
        ap.add_argument("--%s-core" % c, type=int, default=0)
        ap.add_argument("--%s-addr" % c, type=lambda s: int(s, 0))
    ap.add_argument("--clock", type=int, default=8000000)
    ap.add_argument("--mode", default="both", choices=("both", "seq", "par"))
    ap.add_argument("--rounds", type=int, default=1)
    args = ap.parse_args()

    with Esw(args.port, timeout=120) as d:
        d.window = 4   # capped by the board: 4 per channel on proto >= 4, else 3
        print(d.info()["fw"])
        jobs = []
        for ch, c in ((0, "a"), (1, "b")):
            core = getattr(args, c + "_core")
            d.hold(ch, P.GROUP_SWD)
            d.wait_group(ch, P.GROUP_SWD, "CLOSED", 2.0)
            info = d.attach(ch, args.clock, 1, core=core)
            d.detach(ch)
            d.release(ch, P.GROUP_SWD)
            runs = load(getattr(args, c), getattr(args, c + "_addr"), info["regions"])
            size = sum(len(b) for _, b in runs)
            print("channel %s: %s, %s: %d KB in %d run(s)" % ("AB"[ch], info["part"], os.path.basename(getattr(args, c)),
                                                              size >> 10, len(runs)))
            jobs.append((ch, core, runs))

        for r in range(args.rounds):
            seq_out, seq = {}, None
            if args.mode in ("both", "seq"):
                t0 = time.monotonic()
                for ch, core, runs in jobs:
                    flash_channel(d, ch, core, runs, args.clock, seq_out)
                seq = time.monotonic() - t0
                print("sequential: A %.2f s + B %.2f s = %.2f s" % (seq_out["A"], seq_out["B"], seq))
            if args.mode in ("both", "par"):
                out, errs, t0 = {}, [], time.monotonic()

                def worker(job):
                    try:
                        flash_channel(d, job[0], job[1], job[2], args.clock, out)
                    except Exception as e:  # noqa: BLE001
                        errs.append("channel %s: %s" % ("AB"[job[0]], e))
                threads = [threading.Thread(target=worker, args=(j,)) for j in jobs]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()
                par = time.monotonic() - t0
                if errs:
                    print("parallel: FAILED:", "; ".join(errs))
                    return 1
                line = "parallel:   A %.2f s, B %.2f s, wall %.2f s" % (out["A"], out["B"], par)
                if seq:
                    line += "  -> %.2fx faster than sequential (ideal %.2fx: the slower channel alone)" % (
                        seq / par, seq / max(seq_out.values()))
                print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())

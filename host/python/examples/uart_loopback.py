#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd self-test: both DUT UARTs at once, full duplex, through the USB link, with no DUT and no wiring.

    ../../../.venv/bin/python uart_loopback.py [--baud 1000000] [--seconds 10] [--channels AB] [--port ...]

Each channel's UART runs in internal loopback (ESW_UART_LOOPBACK: TX looped to RX inside the ESP32-S3 UART; the
pads and the isolation switches are not touched). The host streams random data into both channels at once and
checks that every byte comes back in order. That loads the link exactly like two DUTs talking full duplex at that
baud rate: per channel, baud/10 bytes/s go host -> board and the same back.
Works directly on the port or through a running `esw serve` (then the TTY bridge must leave the channels to us:
`esw serve --uart-a off --uart-b off`, or use tty_loopback.py which tests the TTYs themselves).
"""
import argparse
import os
import random
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw  # noqa: E402


def run_channel(d, ch, baud, seconds, out):
    rate = baud / 10.0                       # bytes/s on the wire, 8N1
    total = int(rate * seconds)
    data = random.Random(ch).randbytes(total)
    got = bytearray()
    errs = []

    def reader():
        deadline = time.monotonic() + seconds + 10
        while len(got) < total and time.monotonic() < deadline:
            got.extend(d.uart_read(ch, None, timeout=0.5))

    t = threading.Thread(target=reader)
    t0 = time.monotonic()
    t.start()
    try:
        d.uart_write(ch, data, timeout=seconds + 30)
    except Exception as e:  # noqa: BLE001
        errs.append(str(e))
    t.join()
    dt = time.monotonic() - t0
    bad = next((i for i, (a, b) in enumerate(zip(data, got)) if a != b), None)
    out[ch] = dict(sent=total, got=len(got), first_bad=bad, seconds=dt, errs=errs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--channels", default="AB")
    args = ap.parse_args()
    chans = ["AB".index(c) for c in args.channels.upper()]
    ok = True
    with Esw(args.port, timeout=10) as d:
        print(d.info()["fw"], "via", d.port)
        for ch in chans:
            actual = d.uart_open(ch, args.baud, loopback=True)
            print("channel %s: loopback %d baud (%s back-end)" % ("AB"[ch], actual, d.uart_backend[ch]))
            d.uart_clear(ch)
        out, ths = {}, []
        for ch in chans:
            ths.append(threading.Thread(target=run_channel, args=(d, ch, args.baud, args.seconds, out)))
        for t in ths:
            t.start()
        for t in ths:
            t.join()
        for ch in chans:
            r, st = out[ch], d.uart_stats(ch)
            good = r["got"] == r["sent"] and r["first_bad"] is None and not r["errs"]
            ok &= good
            print("channel %s: %s  %d/%d bytes back in %.1f s = %.1f KB/s each way;  board: fifo_ovf %d "
                  "buf_full %d usb_drop %d frame_err %d  host flags 0x%X%s" % (
                      "AB"[ch], "OK" if good else "FAIL", r["got"], r["sent"], r["seconds"],
                      r["got"] / r["seconds"] / 1024, st["fifo_ovf"], st["buf_full"], st["usb_drop"],
                      st["frame_err"], d.uart_flags[ch],
                      "" if r["first_bad"] is None else "  first bad byte at %d" % r["first_bad"]))
            for e in r["errs"]:
                print("   ", e)
            d.uart_close(ch)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

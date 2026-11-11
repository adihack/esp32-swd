#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd test of the TTY bridge: both TTYs at once, full duplex, through PTY -> daemon -> USB -> UART.

    esw serve --test-loopback &                 # bridge opens the UARTs in internal loopback (no DUT, no wiring)
    ../../../.venv/bin/python tty_loopback.py [--baud 1000000] [--seconds 10] [--channels AB]

    esw serve &                                 # real pads: J302 TX -> J402 RX and J402 TX -> J302 RX (GND common)
    ../../../.venv/bin/python tty_loopback.py --cross [--baud 1000000]

Opens <run dir>/<board>/ttyA and ttyB with pyserial like any terminal program would (the baud rate set here is
what the bridge configures on the board) and streams random data into both at once.
- default: every byte must come back on the same TTY (internal loopback, or a TX-RX jumper on one header);
- --cross: what goes into ttyA must come out of ttyB and the other way round, through both channels' isolation
  switches. The interlock closes them only with each channel's DUT powered (VTref on J301/J401).
"""
import argparse
import os
import random
import sys
import threading
import time

import serial

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import link  # noqa: E402


def stream(src, dst, data, seconds, out, key):
    """Write data into src, read it from dst; out[key] = result."""
    got = bytearray()

    def reader():
        deadline = time.monotonic() + seconds + 15
        while len(got) < len(data) and time.monotonic() < deadline:
            got.extend(dst.read(65536) if dst.in_waiting else dst.read(1))

    t = threading.Thread(target=reader)
    t0 = time.monotonic()
    t.start()
    for off in range(0, len(data), 4096):
        src.write(data[off:off + 4096])
    t.join()
    dt = time.monotonic() - t0
    bad = next((i for i, (a, b) in enumerate(zip(data, got)) if a != b), None)
    out[key] = dict(sent=len(data), got=len(got), bad=bad, seconds=dt)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="board id (default: the only daemon running)")
    ap.add_argument("--baud", type=int, default=1000000)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--channels", default="AB")
    ap.add_argument("--cross", action="store_true", help="ttyA <-> ttyB through the real pads (cables A<->B)")
    args = ap.parse_args()
    boards = [args.board] if args.board else link.running_daemons()
    if len(boards) != 1:
        sys.exit("need exactly one running daemon (esw serve), found: %s" % (boards or "none"))
    d = link.board_dir(boards[0])
    chans = "AB" if args.cross else args.channels.upper()
    ports = {c: serial.Serial(os.path.join(d, "tty" + c), args.baud, timeout=0.5) for c in chans}
    time.sleep(0.4)          # the bridge sees the opens, configures the UARTs, the switches close
    for p in ports.values():
        p.reset_input_buffer()
    total = int(args.baud / 10 * args.seconds)
    out, ths = {}, []
    for c in chans:
        dst = ("B" if c == "A" else "A") if args.cross else c
        data = random.Random(ord(c) + args.baud).randbytes(total)
        ths.append(threading.Thread(target=stream, args=(ports[c], ports[dst], data, args.seconds, out,
                                                          "tty%s -> tty%s" % (c, dst))))
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    for p in ports.values():
        p.close()
    ok = True
    for k in sorted(out):
        r = out[k]
        good = r["got"] == r["sent"] and r["bad"] is None
        ok &= good
        print("%s: %s  %d/%d bytes in %.1f s = %.1f KB/s%s" % (
            k, "OK" if good else "FAIL", r["got"], r["sent"], r["seconds"], r["got"] / r["seconds"] / 1024,
            "" if r["bad"] is None else ", first bad byte at %d" % r["bad"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

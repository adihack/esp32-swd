#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd UART stress test: both channels at full rate at once, plus SWD traffic and ESP32 flash erases.

Wiring:
    channel A: as for uart_test.py (J302 GND/TX/RX -> nRF52840 DK GND/P1.01/P1.02, SWD cable J301 -> DK P18)
    channel B: J402.2 (TX) jumpered to J402.3 (RX): loopback; J402.1 -> DK GND; J402.4 (VTref) -> DK VDD (~3.0 V)

    ../../../.venv/bin/python uart_stress.py [--duration 60] [--baud-a 1000000] [--baud-b 3000000] [--dma B]

Load, all at the same time for --duration seconds:
  channel A   full duplex with the DK's test firmware (both directions at line rate, independent data)
  channel B   random data through the loopback (TX and RX at line rate)
  SWD         AHB-AP reads on channel A, back to back
  flash       the board erases and writes a 4 KB flash sector every --flash-gap ms (test hook): each erase stops
              the flash cache for about 40 ms, so only IRAM interrupt handlers run and USB stalls - the worst case
              for UART reception (the firmware itself never writes flash while streaming)
Pass: every byte correct on both channels, zero FIFO overflow / buffer-full / DMA / USB-drop counts.
"""
import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, proto as P  # noqa: E402
from uart_test import Dut, xorshift, firmware  # noqa: E402

A, B = 0, 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="device, board id or unix:<socket>; default: the only board (through esw serve if it runs)")
    ap.add_argument("--duration", type=float, default=60)
    ap.add_argument("--baud-a", type=int, default=1000000, help="nRF52840: 1000000 at most")
    ap.add_argument("--baud-b", type=int, default=3000000)
    ap.add_argument("--dma", default="B", choices=("A", "B", "none"), help="which channel gets the DMA engine")
    ap.add_argument("--no-b", action="store_true", help="channel B not wired: stress channel A only")
    ap.add_argument("--no-flash-stress", action="store_true")
    ap.add_argument("--flash-gap", type=int, default=250, help="ms between flash sector erases (0: back to back)")
    ap.add_argument("--no-swd", action="store_true")
    args = ap.parse_args()

    d = Esw(args.port, timeout=30)
    d.window = 3
    stop = threading.Event()
    res = {"a_bytes": 0, "a_bad": 0, "a_dk_err": 0, "a_rounds": 0, "b_bytes": 0, "b_bad": 0, "b_rounds": 0,
           "swd_reads": 0, "errors": []}

    d.hold(A, P.GROUP_SWD)
    if d.wait_group(A, P.GROUP_SWD, "CLOSED", 2.0)["swd"] != "CLOSED":
        sys.exit("channel A: SWD group not closed (DK powered?)")
    d.attach(A)
    d.flash(A, 0, firmware())
    chans = [A] if args.no_b else [A, B]
    # the DMA engine goes to the first channel opened while it is free
    order = [B, A] if args.dma == "B" else [A, B]
    for ch in order:
        if ch not in chans:
            continue
        dma = args.dma == "AB"[ch]
        baud = 115200 if ch == A else args.baud_b
        d.uart_open(ch, baud, dma=dma)
    for ch in chans:
        st = d.wait_group(ch, P.GROUP_UART, "CLOSED", 2.0)
        if st["uart"] != "CLOSED":
            sys.exit("channel %s: UART group not closed (VTref %s mV): check the wiring" % ("AB"[ch], st["vtref_mv"]))
    d.uart_clear(A)
    d.reset(A)
    dut = Dut(d, A)
    print("DK:", dut.line())
    if args.baud_a != 115200:
        assert dut.cmd("baud %d" % args.baud_a) == "ok baud %d" % args.baud_a
        d.uart_open(A, args.baud_a, dma=args.dma == "A")
        time.sleep(0.02)
        assert dut.cmd("ping") == "pong"
    for ch in chans:
        s = d.uart_stats(ch)
        print("channel %s: %d baud, %s back-end" % ("AB"[ch], s["baud"], s["backend"]))

    def chan_a():
        n, r = args.baud_a // 10 * 2, 0
        while not stop.is_set():
            st_, sr_ = 0x1000 + r, 0x2000 + r
            to_dk, from_dk = xorshift(sr_, n), xorshift(st_, n)
            try:
                dut.cmd("duplex %d %d %d" % (n, st_, sr_), timeout=3)
                d.uart_write(A, to_dk)
                got = d.uart_read(A, n, timeout=3 * n / (args.baud_a / 10) + 3)
                rep = dut.line(3)
            except Exception as e:  # noqa: BLE001
                res["errors"].append("A round %d: %s" % (r, e))
                return
            res["a_bad"] += sum(1 for x, y in zip(got, from_dk) if x != y) + n - len(got)
            res["a_dk_err"] += int(rep.split("err=")[1].split()[0]) if "err=" in rep else n
            res["a_bytes"] += n
            res["a_rounds"] = r = r + 1

    def chan_b():
        n, r = 64 * 1024, 0
        while not stop.is_set():
            data = os.urandom(n)
            try:
                d.uart_write(B, data)
                got = d.uart_read(B, n, timeout=3 * n / (args.baud_b / 10) + 3)
            except Exception as e:  # noqa: BLE001
                res["errors"].append("B round %d: %s" % (r, e))
                return
            res["b_bad"] += sum(1 for x, y in zip(got, data) if x != y) + n - len(got)
            res["b_bytes"] += n
            res["b_rounds"] = r = r + 1

    def swd():
        while not stop.is_set():
            try:
                d.mem_read(A, 0x20000000, 256)
                res["swd_reads"] += 1
            except Exception as e:  # noqa: BLE001
                res["errors"].append("SWD: %s" % e)
                return

    threads = [threading.Thread(target=chan_a)] + ([] if args.no_b else [threading.Thread(target=chan_b)])
    if not args.no_swd:
        threads.append(threading.Thread(target=swd))
    if not args.no_flash_stress:
        d.test_flash_stress(int(args.duration * 1000), args.flash_gap)
    t0 = time.monotonic()
    for t in threads:
        t.start()
    while time.monotonic() - t0 < args.duration and all(t.is_alive() for t in threads):
        time.sleep(1)
        print("\r  %3.0f s: A %d KB (bad %d, DK err %d), B %d KB (bad %d), SWD reads %d   " % (
            time.monotonic() - t0, res["a_bytes"] // 1024, res["a_bad"], res["a_dk_err"], res["b_bytes"] // 1024,
            res["b_bad"], res["swd_reads"]), end="", flush=True)
    stop.set()
    for t in threads:
        t.join(timeout=30)
    dt = time.monotonic() - t0
    print()
    ops = d.test_flash_stress(0)[0] if not args.no_flash_stress else 0
    ok = not res["errors"] and res["a_bad"] == res["a_dk_err"] == res["b_bad"] == 0
    for ch in chans:
        s = d.uart_stats(ch)
        lost = s["fifo_ovf"] + s["buf_full"] + s["dma_err"] + s["usb_drop"] + s["frame_err"] + s["parity_err"]
        ok = ok and lost == 0
        print("channel %s (%s, %d baud): rx %d, tx %d | fifo_ovf %d, buf_full %d, dma_err %d, usb_drop %d, "
              "framing %d, parity %d" % ("AB"[ch], s["backend"], s["baud"], s["rx"], s["tx"], s["fifo_ovf"],
                                         s["buf_full"], s["dma_err"], s["usb_drop"], s["frame_err"], s["parity_err"]))
    print("%.0f s: A %.1f MB each way (%d bad, DK %d), B %.1f MB each way (%d bad), %d SWD reads, %d flash erases"
          % (dt, res["a_bytes"] / 2**20, res["a_bad"], res["a_dk_err"], res["b_bytes"] / 2**20, res["b_bad"],
             res["swd_reads"], ops))
    for e in res["errors"]:
        print("  error:", e)
    try:
        dut.cmd("baud 115200")
    except Exception:  # noqa: BLE001
        pass
    for ch in chans:
        d.uart_close(ch)
    d.detach(A)
    d.release(A, P.GROUP_SWD)
    d.close()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

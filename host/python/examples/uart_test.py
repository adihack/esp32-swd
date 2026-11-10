#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd UART test on hardware: r5 channel A + nRF52840 DK running testfw/nrf52840_uart (flashed over SWD here).

Wiring, besides the SWD cable (r5 J301 -> DK P18 "Debug in"):
    r5 J302.1 GND -> DK GND
    r5 J302.2 TX  -> DK P1.01 (Arduino header D0: the test firmware's RX)
    r5 J302.3 RX  -> DK P1.02 (Arduino header D1: the test firmware's TX)
    r5 J302.4 VTref: leave open (the SWD cable carries VTref)

    ../../../.venv/bin/python uart_test.py [--bauds 115200,460800,1000000] [--stream 1M] [--no-flash]

1. flash the test firmware over SWD (erase the touched page, program, CRC verify on the target)
2. open the UART: the firmware closes the UART switches only with VTref valid
3. reset the DK over SWD and catch its boot banner from the first byte
4. per baud rate: switch both ends, ping, echo random data (tool TX -> DK -> tool RX, byte-exact)
5. full duplex at the last baud rate: both ends send independent streams at once, each end checks what it gets
   (our TX edges once coupled into our RX over jumper wires: see esw_uart.h)
6. DK -> tool stream (xorshift32, byte-exact) while SWD memory reads run on the same channel, and the host stops
   reading for 250 ms in the middle: the board must buffer, not drop
7. tool -> DK: random data, CRC-32 computed on the DK
8. isolation: with the DK printing ticks, release the UART group: no byte arrives and writes are refused; hold it
   again: the ticks resume (the DK kept counting) and commands work, so both pads were re-routed
9. error counters on both ends
10. detach: the nRESET pulse reaches the DK (banner shows a pin reset)
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, EswError, proto as P  # noqa: E402

FW_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "testfw", "nrf52840_uart")
RESULTS = []


def check(name, ok, info=""):
    RESULTS.append((name, bool(ok)))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  " + info) if info else ""))
    return ok


def size_arg(s):
    s = s.upper()
    mul = 1 << 20 if s.endswith("M") else 1 << 10 if s.endswith("K") else 1
    return int(s.rstrip("MK")) * mul


def xorshift(seed, n):
    """The DK's `send` stream: xorshift32 (13, 17, 5), 4 bytes little endian per step."""
    x, out = seed or 1, bytearray()
    while len(out) < n:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out += struct.pack("<I", x)
    return bytes(out[:n])


def nrf_baud(baud):
    """What the nRF52840 UART really runs at: 16 MHz / k."""
    return 16000000 / round(16000000 / baud)


def firmware():
    binf = os.path.join(FW_DIR, "build", "uart_test.bin")
    srcs = [os.path.join(FW_DIR, f) for f in ("main.c", "link.ld", "build.py")]
    if not os.path.exists(binf) or max(map(os.path.getmtime, srcs)) > os.path.getmtime(binf):
        subprocess.run([sys.executable, os.path.join(FW_DIR, "build.py")], check=True)
    with open(binf, "rb") as f:
        return f.read()


class Dut:
    """The test firmware's line protocol over the esp-swd UART."""

    def __init__(self, d, ch):
        self.d, self.ch = d, ch

    def line(self, timeout=2.0):
        while True:
            text = self.d.uart_read_until(self.ch, b"\n", timeout).decode("latin-1").strip()
            if text:
                return text

    def cmd(self, text, timeout=2.0):
        self.d.uart_write(self.ch, (text + "\n").encode())
        return self.line(timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--channel", default="A", choices="AB")
    ap.add_argument("--bauds", default="115200,460800,1000000")
    ap.add_argument("--stream", type=size_arg, default=1 << 20, help="DK -> tool stream size (default 1M)")
    ap.add_argument("--sink", type=size_arg, default=256 << 10, help="tool -> DK size (default 256K)")
    ap.add_argument("--no-flash", action="store_true", help="the DK already runs the test firmware")
    ap.add_argument("--no-dma", action="store_true", help="keep the channel on the interrupt-driven back-end")
    args = ap.parse_args()
    ch = "AB".index(args.channel)
    bauds = [int(float(b)) for b in args.bauds.split(",")]
    image = None if args.no_flash else firmware()
    ukw = dict(dma=not args.no_dma)

    d = Esw(args.port, timeout=30)
    d.window = 3
    info = d.info()
    print("%s, protocol %d" % (info["fw"], info["proto"]))
    if info["proto"] < 2:
        sys.exit("the board firmware has no UART bridge (protocol %d): flash app/ first" % info["proto"])
    totals = dict(rx=0, tx=0, usb_drop=0, fifo_ovf=0, buf_full=0, frame_err=0, parity_err=0, brk=0, dma_err=0)

    def add_stats():
        s = d.uart_stats(ch)
        for k in totals:
            totals[k] += s[k]

    d.hold(ch, P.GROUP_SWD)
    st = d.wait_group(ch, P.GROUP_SWD, "CLOSED", 2.0)
    if st["swd"] != "CLOSED":
        d.release(ch, P.GROUP_SWD)
        sys.exit("SWD group not closed (VTref %s): is the DK powered?" % st["vtref_mv"])
    attached = False
    try:
        t = d.attach(ch)
        attached = True
        print("%s, VTref %d mV, SWD %d kHz\n" % (t["part"], st["vtref_mv"], t["khz"]))

        print("1. flash")
        if image:
            t0 = time.monotonic()
            d.flash(ch, 0, image)
            check("test firmware programmed and CRC-verified", True,
                  "%d bytes in %.2f s" % (len(image), time.monotonic() - t0))

        print("2-3. UART open, boot banner")
        actual = d.uart_open(ch, 115200, **ukw)
        st = d.wait_group(ch, P.GROUP_UART, "CLOSED", 2.0)
        if not check("UART group closed by the interlock", st["uart"] == "CLOSED",
                     "VTref %s mV, tool UART %d baud, %s back-end" % (st["vtref_mv"], actual,
                                                                       d.uart_backend[ch])):
            return 1
        d.uart_clear(ch)
        d.reset(ch)
        dut = Dut(d, ch)
        try:
            banner = dut.line(2.0)
        except TimeoutError as e:
            check("boot banner after an SWD reset", False, str(e))
            print("\n  Nothing arrived from the DK: check J302.3 (tool RX) -> DK P1.02 and GND.")
            return 1
        check("boot banner after an SWD reset", banner.startswith("ESWTEST"), repr(banner))
        r = dut.cmd("ping")
        if not check("ping -> pong (tool TX -> DK)", r == "pong", repr(r)):
            print("\n  The DK answers nothing: check J302.2 (tool TX) -> DK P1.01.")
            return 1

        print("4. echo")
        cur = 115200
        for baud in bauds:
            if baud != cur:
                r = dut.cmd("baud %d" % baud)
                if not check("DK switches to %d baud" % baud, r == "ok baud %d" % baud, repr(r)):
                    break
                add_stats()
                actual = d.uart_open(ch, baud, **ukw)
                cur = baud
                time.sleep(0.01)
                r = dut.cmd("ping")
                check("ping at %d" % baud, r == "pong", "tool %d baud, nRF %.0f baud (%+.2f %%)"
                      % (actual, nrf_baud(baud), 100 * (nrf_baud(baud) - actual) / actual))
            size = max(4096, baud // 10)   # about 1 s per direction
            data = os.urandom(size)
            r = dut.cmd("echo %d" % size)
            t0 = time.monotonic()
            d.uart_write(ch, data)
            back = d.uart_read(ch, size, timeout=2 * size / (baud / 10) + 2)
            dt = time.monotonic() - t0
            try:
                tail = dut.line(2.0)
            except TimeoutError:
                tail = "(none)"
            diff = next((i for i, (a, b) in enumerate(zip(back, data)) if a != b), None)
            check("echo %d KB at %d baud" % (size // 1024, baud), back == data and tail == "done",
                  "%.2f s, %.1f KB/s each way%s" % (dt, size / 1024 / dt, "" if back == data else
                                                     ", got %d bytes, first difference at %s, DK said %r"
                                                     % (len(back), diff, tail)))

        print("5. full duplex")
        n, st_, sr_ = max(4096, cur // 10 * 2), 0x1234, 0x5678
        to_dk, from_dk = xorshift(sr_, n), xorshift(st_, n)
        r = dut.cmd("duplex %d %d %d" % (n, st_, sr_))
        t0 = time.monotonic()
        d.uart_write(ch, to_dk)
        got = d.uart_read(ch, n, timeout=2 * n / (cur / 10) + 3)
        dt = time.monotonic() - t0
        try:
            rep = dut.line(3.0)
        except TimeoutError:
            rep = "(none)"
        bad = sum(1 for a, b in zip(got, from_dk) if a != b) + n - len(got)
        check("full duplex %d KB each way at %d baud" % (n // 1024, cur),
              bad == 0 and rep == "dup got=%d err=0" % n,
              "%.2f s; DK -> tool %d bad bytes; DK says %r" % (dt, bad, rep))

        print("6. DK -> tool stream with SWD traffic and a host stall")
        n, seed = args.stream, 0x2545F491
        swd = {"reads": 0, "err": None}
        stop = threading.Event()

        def swd_load():
            while not stop.is_set():
                try:
                    d.mem_read(ch, 0x20000000, 256)
                    swd["reads"] += 1
                except Exception as e:  # noqa: BLE001
                    swd["err"] = e
                    return

        stalled = []
        deliver = d._uart_event

        def stall_once(*a):   # runs on the client's reader thread: blocks all USB reading for 250 ms
            if not stalled and d.uart_pending(ch) > n // 2:
                stalled.append(time.monotonic())
                time.sleep(0.25)
            deliver(*a)

        d._uart_event = stall_once
        want = xorshift(seed, n)
        r = dut.cmd("send %d %d" % (n, seed))
        th = threading.Thread(target=swd_load)
        t0 = time.monotonic()
        th.start()
        got = d.uart_read(ch, n, timeout=1.5 * n / (cur / 10) + 3)
        dt = time.monotonic() - t0
        stop.set()
        th.join()
        d._uart_event = deliver
        try:
            tail = dut.line(2.0)
        except TimeoutError:
            tail = "(none)"
        diff = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), None)
        s = d.uart_stats(ch)
        check("stream %d KB at %d baud, byte-exact through a 250 ms host stall" % (n // 1024, cur),
              got == want and tail == "done" and bool(stalled) and s["usb_drop"] == 0 and s["buf_full"] == 0,
              "%.2f s, %.1f KB/s, stalled %s, usb_drop %d%s" % (
                  dt, len(got) / 1024 / dt, "yes" if stalled else "no", s["usb_drop"], "" if got == want else
                  ", got %d bytes, first difference at %s" % (len(got), diff)))
        check("SWD reads during the stream", swd["reads"] > 0 and swd["err"] is None,
              "%d x 1 KB AHB-AP reads%s" % (swd["reads"], "" if swd["err"] is None else ", error %s" % swd["err"]))

        print("7. tool -> DK")
        data = os.urandom(args.sink)
        r = dut.cmd("sink %d" % len(data))
        t0 = time.monotonic()
        d.uart_write(ch, data)
        r = dut.line(2 * len(data) / (cur / 10) + 3)
        dt = time.monotonic() - t0
        check("%d KB to the DK at %d baud, CRC-32 on the DK" % (len(data) // 1024, cur),
              r == "crc %08x" % zlib.crc32(data), "%.2f s, %.1f KB/s, DK %r" % (dt, len(data) / 1024 / dt, r))

        print("8. isolation")
        r = dut.cmd("tick 10")
        ticks = [dut.line() for _ in range(5)]
        k1 = int(re.match(r"tick (\d+) ", ticks[-1]).group(1))
        add_stats()
        d.release(ch, P.GROUP_UART)
        st = d.wait_group(ch, P.GROUP_UART, "OPEN", 1.0)
        time.sleep(0.05)
        d.uart_clear(ch)
        time.sleep(0.5)
        leaked = d.uart_pending(ch)
        try:
            d.uart_write(ch, b"\n")
            refused = "accepted"
        except EswError as e:
            refused = "refused" if e.status == P.ERR_INTERLOCK else str(e)
        check("released: switches open, nothing received, writes refused",
              st["uart"] == "OPEN" and leaked == 0 and refused == "refused",
              "group %s, %d bytes in 0.5 s, write %s" % (st["uart"], leaked, refused))
        d.hold(ch, P.GROUP_UART)
        st = d.wait_group(ch, P.GROUP_UART, "CLOSED", 2.0)
        k2, t_end = None, time.monotonic() + 2
        while k2 is None and time.monotonic() < t_end:
            m = re.match(r"tick (\d+) (\d+)$", dut.line())
            k2 = int(m.group(1)) if m else None
        check("held again: ticks resume, the DK kept running", k2 is not None and k2 - k1 >= 40,
              "tick %s -> %s" % (k1, k2))
        d.uart_write(ch, b"\n")   # stops the ticks
        time.sleep(0.05)
        d.uart_clear(ch)
        r = dut.cmd("ping")
        check("TX re-routed after the closing", r == "pong", repr(r))

        print("9. error counters")
        r = dut.cmd("stat")
        add_stats()
        dk = dict(kv.split("=") for kv in r.split()[1:]) if r.startswith("stat") else {}
        check("tool: no lost bytes (UART FIFO, RX buffer, USB)",
              totals["fifo_ovf"] == totals["buf_full"] == totals["usb_drop"] == totals["dma_err"] == 0 and
              not d.uart_flags[ch] & P.UART_RX_LOST,
              "rx %d, tx %d, fifo_ovf %d, buf_full %d, usb_drop %d, dma_err %d"
              % (totals["rx"], totals["tx"], totals["fifo_ovf"], totals["buf_full"], totals["usb_drop"],
                 totals["dma_err"]))
        check("tool: line errors at most 2 (from cutting the line in step 8)",
              totals["frame_err"] + totals["parity_err"] + totals["brk"] <= 2,
              "framing %d, parity %d, break %d" % (totals["frame_err"], totals["parity_err"], totals["brk"]))
        check("DK: no receive errors", dk and all(dk[k] == "0" for k in ("ovr", "par", "frm", "brk")), r)

        print("10. detach: nRESET")
        r = dut.cmd("baud 115200")
        d.uart_open(ch, 115200, **ukw)
        time.sleep(0.01)
        d.uart_clear(ch)
        d.detach(ch)
        attached = False
        junk, banner, t_end = [], "(none)", time.monotonic() + 2
        while time.monotonic() < t_end:
            try:
                text = dut.line(max(0.01, t_end - time.monotonic()))
            except TimeoutError:
                break
            if text.startswith("ESWTEST"):
                banner = text
                break
            junk.append(text)
        check("pin reset reaches the DK", banner.endswith("resetreas=0x00000001"),
              "%r%s" % (banner, ", before it: %r" % junk if junk else ""))
    finally:
        try:
            d.uart_close(ch)
            if attached:
                d.detach(ch)
            d.release(ch, P.GROUP_SWD)
            st = d.wait_group(ch, P.GROUP_SWD, "OPEN", 1.0)
            print("\nreleased: %s" % st)
        finally:
            d.close()
    failed = [n for n, ok in RESULTS if not ok]
    print("\n%d/%d passed%s" % (len(RESULTS) - len(failed), len(RESULTS), "" if not failed else "; FAILED: " +
                                "; ".join(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

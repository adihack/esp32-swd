# SPDX-License-Identifier: MIT
"""esw: command-line tool for the esp-swd board.

  esw info                                  board, firmware, both channels (switches, modes), daemon and TTYs
  esw flash A=app.hex [B=fw.bin@0x08000000] channels in parallel; A1=net.hex: channel A, core 1 (nRF5340 network)
  esw reset A                               pin reset (attach + detach)
  esw mode A uart on|off|auto               isolation switch modes (swd, uart or all; channels A, B or AB)
  esw isolate [AB]                          every switch of the channel(s) off until set back to auto
  esw serve                                 daemon: TTY bridge (ttyA/ttyB) + shared access for every other command
  esw stop                                  stop the daemon (the board isolates what it held)
  esw tty [A off|open|always]               TTY bridge paths and state; change a channel's bridge policy
  esw update app/build | merged.bin         update the board's firmware over USB (esptool, ROM downloader)
  esw reboot [--rom]

Exit status: 0 success, 1 failure, 2 usage error. --port selects the board (device path or USB serial number) when
more than one is attached; with a daemon running, every command goes through it.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import threading
import time

from . import link
from . import proto as P
from . import server
from .client import Esw, EswError

CH = {"A": 0, "B": 1}


def _chans(s):
    s = s.upper()
    if not s or any(c not in "AB" for c in s):
        raise argparse.ArgumentTypeError("channel must be A, B or AB")
    return [CH[c] for c in dict.fromkeys(s)]


def _vt(st):
    return "%d mV" % st["vtref_mv"] if st["vtref_mv"] is not None else "not measured"


def cmd_info(a):
    with Esw(a.port) as d:
        i = d.info()
        print("board     %s via %s" % (link.usb_serial(d.port) if not d.port.startswith("unix:") else
                                      os.path.basename(os.path.dirname(d.port[5:])), d.port))
        print("firmware  %s (protocol %d, up %.0f s, heap %d)" % (i["fw"], i["proto"], i["uptime_ms"] / 1000,
                                                                  i["free_heap"]))
        for ch in (0, 1):
            st = d.state(ch)
            print("channel %s VTref %-12s SWD %-10s (mode %s%s)  UART %-10s (mode %s%s)" % (
                "AB"[ch], _vt(st), st["swd"], st.get("mode_swd", "?"), ", attached" if st.get("attached") else "",
                st["uart"], st.get("mode_uart", "?"),
                ", stream" + (" loopback" if st.get("uart_loop") else "") if st.get("uart_open") else ""))
        if d.port.startswith("unix:"):
            s = json.loads(d.daemon(server.OP_STATUS))
            print("daemon    %s, %d client(s)" % (s["socket"], s["clients"]))
            for ch, t in enumerate(s["tty"]):
                print("TTY %s     %s -> %s  policy %s, %s, uart %s%s  rx %d tx %d dropped %d/%d" % (
                    "AB"[ch], t["path"], t["pts"], t["policy"], "open" if t["open"] else "closed",
                    "%d %s%s" % (t["settings"][0], t["settings"][1], " 2stop" if t["settings"][2] else "")
                    if t["uart"] else "off", " (%s)" % t["note"] if t["note"] else "", t["rx"], t["tx"],
                    t["rx_dropped"], t["tx_dropped"]))
        else:
            print("daemon    not running (TTYs: `esw serve`)")
    return 0


def _parse_spec(s):
    """A=app.hex, A1=net.hex, B=fw.bin@0x08000000 -> (ch, core, path, addr)."""
    if "=" not in s:
        raise argparse.ArgumentTypeError("%r: expected CH[CORE]=IMAGE[@ADDR], e.g. A=app.hex" % s)
    k, v = s.split("=", 1)
    k = k.upper()
    if not k or k[0] not in "AB" or (k[1:] and not k[1:].isdigit()):
        raise argparse.ArgumentTypeError("%r: channel must be A or B, optionally followed by a core number" % s)
    path, addr = v, None
    if "@" in v:
        path, a_ = v.rsplit("@", 1)
        addr = int(a_, 0)
    if not os.path.isfile(path):
        raise argparse.ArgumentTypeError("%s: no such file" % path)
    return CH[k[0]], int(k[1:] or 0), path, addr


def cmd_flash(a):
    jobs = {}
    for ch, core, path, addr in a.images:
        jobs.setdefault(ch, []).append((core, path, addr))
    errs, lock, t0 = [], threading.Lock(), time.monotonic()

    def log(ch, msg):
        with lock:
            print("[%s] %s" % ("AB"[ch], msg), flush=True)

    with Esw(a.port, timeout=120) as d:
        d.window = 4

        def run(ch):
            for core, path, addr in jobs[ch]:
                try:
                    r = d.flash_file(ch, path, core, addr, a.clock, a.allow_erase, log=lambda m, ch=ch: log(ch, m))
                    log(ch, "%s: %d bytes (%d data) in %.2f s, verified" % (os.path.basename(path), r["bytes"],
                                                                             r["data"], r["seconds"]))
                except (EswError, OSError, ValueError, TimeoutError) as e:
                    log(ch, "%s: FAILED: %s" % (os.path.basename(path), e))
                    errs.append(e)
                    return

        ths = [threading.Thread(target=run, args=(ch,)) for ch in jobs]
        for t in ths:
            t.start()
        for t in ths:
            t.join()
    print("%s in %.2f s" % ("FAILED" if errs else "done", time.monotonic() - t0))
    return 1 if errs else 0


def cmd_reset(a):
    with Esw(a.port) as d:
        for ch in a.ch:
            d.connect(ch)
            d.disconnect(ch)   # debug off + nRESET pulse: the target runs from reset
            print("channel %s: reset" % "AB"[ch])
    return 0


def cmd_mode(a):
    groups = ("swd", "uart") if a.group == "all" else (a.group,)
    with Esw(a.port) as d:
        for ch in a.ch:
            for g in groups:
                d.mode(ch, g, a.mode)
        time.sleep(0.12)   # one interlock tick plus VTref settling
        for ch in a.ch:
            st = d.state(ch)
            print("channel %s VTref %-12s SWD %-10s (mode %s)  UART %-10s (mode %s)" % (
                "AB"[ch], _vt(st), st["swd"], st["mode_swd"], st["uart"], st["mode_uart"]))
    return 0


def cmd_isolate(a):
    a.group, a.mode = "all", "off"
    return cmd_mode(a)


def cmd_serve(a):
    fmts = []
    for f in (a.format_a, a.format_b):
        f = f.upper()
        if len(f) != 3 or f[0] != "8" or f[1] not in "NEO" or f[2] not in "12":
            print("format must be 8N1, 8E1, 8O1, 8N2, ...: %s" % f, file=sys.stderr)
            return 2
        fmts.append((f[1], f[2] == "2"))
    try:
        return server.run(a.port, (a.uart_a, a.uart_b), tuple(fmts), loopback=a.test_loopback)
    except (RuntimeError, FileNotFoundError) as e:
        print("esw serve: %s" % e, file=sys.stderr)
        return 1


def cmd_tty(a):
    with Esw(a.port) as d:
        if not d.port.startswith("unix:"):
            print("no daemon serves this board: start one with `esw serve`", file=sys.stderr)
            return 1
        if a.policy:
            for ch in a.ch:
                d.daemon(server.OP_TTY_POLICY, bytes([ch, server.POLICIES.index(a.policy)]))
        s = json.loads(d.daemon(server.OP_STATUS))
        for ch, t in enumerate(s["tty"]):
            print("%s  %s  (policy %s, %s)" % ("AB"[ch], t["path"], t["policy"], "open" if t["open"] else "closed"))
    return 0


def cmd_stop(a):
    with Esw(a.port) as d:
        if not d.port.startswith("unix:"):
            print("no daemon serves this board", file=sys.stderr)
            return 1
        d.daemon(server.OP_QUIT)
    print("daemon stopped")
    return 0


def _esptool():
    try:
        import esptool  # noqa: F401
        return [sys.executable, "-m", "esptool"]
    except ImportError:
        for name in ("esptool", "esptool.py"):
            for d in os.environ.get("PATH", "").split(os.pathsep):
                if os.access(os.path.join(d, name), os.X_OK):
                    return [os.path.join(d, name)]
    raise FileNotFoundError("esptool not found: pip install esptool")


def _flash_list(path):
    """[(offset, file)] to write: from an ESP-IDF build directory (its flash_args), or a merged image at 0."""
    if os.path.isdir(path):
        fa = os.path.join(path, "flash_args")
        if not os.path.exists(fa):
            raise ValueError("%s: no flash_args (not an ESP-IDF build directory)" % path)
        out = []
        for line in open(fa):
            parts = line.split()
            if len(parts) == 2 and parts[0].startswith("0x"):
                out.append((int(parts[0], 16), os.path.join(path, parts[1])))
        return out
    with open(path, "rb") as f:
        head = f.read(0x8002)
    if len(head) < 0x8002 or head[0] != 0xE9 or head[0x8000:0x8002] != b"\xaa\x50":
        raise ValueError("%s: not a merged image (bootloader at 0x0, partition table at 0x8000). Give the build "
                         "directory, or make one with `idf.py merge-bin`" % path)
    return [(0, path)]


def cmd_update(a):
    try:
        files = _flash_list(a.firmware)
        tool = _esptool()
    except (ValueError, FileNotFoundError) as e:
        print("esw update: %s" % e, file=sys.stderr)
        return 1
    kind, where = link.resolve(a.port)
    daemon = kind == "unix"
    board = os.path.basename(os.path.dirname(where)) if daemon else link.usb_serial(where)
    if daemon:
        with Esw(a.port) as d:
            d.daemon(server.OP_SUSPEND)
        time.sleep(0.3)
    dev = link.find_boards().get(board) if board else where
    if not dev:
        print("esw update: board %s not found" % board, file=sys.stderr)
        return 1
    cmd = tool + ["--chip", "esp32s3", "-p", dev, "--before", "default-reset", "--after", "hard-reset",
                  "write-flash"]
    for off, f in files:
        cmd += ["0x%x" % off, f]
    print("$", " ".join(cmd), flush=True)
    rc = subprocess.call(cmd)
    t0 = time.monotonic()
    while board and time.monotonic() - t0 < 15 and board not in link.find_boards():
        time.sleep(0.2)
    if daemon:
        with Esw("unix:" + where) as d:
            st, body = d.request(P.DAEMON, bytes([server.OP_RESUME]), timeout=20, check=False)
            if st:
                print("daemon could not reconnect: %s" % body.decode(errors="replace"), file=sys.stderr)
    if rc:
        print("esw update: esptool failed (%d). The ROM downloader stays reachable over USB: run it again" % rc,
              file=sys.stderr)
        return 1
    time.sleep(0.5)
    with Esw(a.port) as d:
        print("running:", d.info()["fw"])
    return 0


def cmd_reboot(a):
    with Esw(a.port) as d:
        d.reboot(rom_download=a.rom)
    print("rebooting%s" % (" into the ROM downloader" if a.rom else ""))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="esw", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="device path, USB serial number or unix:<socket> (default: the only board)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="board, channels, daemon").set_defaults(fn=cmd_info)
    p = sub.add_parser("flash", help="flash images, channels in parallel")
    p.add_argument("images", nargs="+", type=_parse_spec, metavar="CH[CORE]=IMAGE[@ADDR]")
    p.add_argument("--clock", type=int, default=8000000, help="SWCLK in Hz (default 8 MHz)")
    p.add_argument("--allow-erase", action="store_true", help="unlock a protected part by mass erase")
    p.set_defaults(fn=cmd_flash)
    p = sub.add_parser("reset", help="pin reset the target(s)")
    p.add_argument("ch", type=_chans)
    p.set_defaults(fn=cmd_reset)
    p = sub.add_parser("mode", help="isolation switch mode")
    p.add_argument("ch", type=_chans)
    p.add_argument("group", choices=("swd", "uart", "all"))
    p.add_argument("mode", choices=tuple(P.MODES))
    p.set_defaults(fn=cmd_mode)
    p = sub.add_parser("isolate", help="all switches of the channel(s) off")
    p.add_argument("ch", type=_chans, nargs="?", default=[0, 1])
    p.set_defaults(fn=cmd_isolate)
    p = sub.add_parser("serve", help="run the daemon (TTY bridge + shared access) in the foreground")
    p.add_argument("--uart-a", choices=server.POLICIES, default="open", help="TTY bridge policy for channel A")
    p.add_argument("--uart-b", choices=server.POLICIES, default="open", help="TTY bridge policy for channel B")
    p.add_argument("--format-a", default="8N1", help="parity/stop bits for A (the baud rate comes from the TTY)")
    p.add_argument("--format-b", default="8N1")
    p.add_argument("--test-loopback", action="store_true", help=argparse.SUPPRESS)   # tty_loopback.py self-test
    p.set_defaults(fn=cmd_serve)
    sub.add_parser("stop", help="stop the daemon").set_defaults(fn=cmd_stop)
    p = sub.add_parser("tty", help="TTY bridge state; set a channel's policy")
    p.add_argument("ch", type=_chans, nargs="?", default=[0, 1])
    p.add_argument("policy", nargs="?", choices=server.POLICIES)
    p.set_defaults(fn=cmd_tty)
    p = sub.add_parser("update", help="update the board firmware (ESP-IDF build directory or merged image)")
    p.add_argument("firmware")
    p.set_defaults(fn=cmd_update)
    p = sub.add_parser("reboot", help="reboot the board")
    p.add_argument("--rom", action="store_true", help="into the ROM downloader (esptool)")
    p.set_defaults(fn=cmd_reboot)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except (EswError, OSError, TimeoutError, ConnectionError) as e:
        print("esw %s: %s" % (a.cmd, e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

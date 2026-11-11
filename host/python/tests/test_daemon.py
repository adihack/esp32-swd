# SPDX-License-Identifier: MIT
"""The host daemon (esw serve) against a simulated board on a PTY: no hardware needed (Linux).

    python -m unittest discover -s host/python/tests -v
"""
import os
import struct
import sys
import tempfile
import threading
import time
import unittest

import serial

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, EswError, proto as P, server  # noqa: E402


class FakeBoard:
    """Answers the framed protocol on the master side of a PTY; UARTs loop TX back as RX events."""

    def __init__(self):
        self.master, slave = os.openpty()
        self.path = os.ttyname(slave)
        os.close(slave)
        self.t0 = time.monotonic()
        self.boot_id = 0x1234
        self.log = []                # (cmd, args) received
        self.drop = set()            # commands to swallow once (lost frames)
        self.uart = {}               # ch -> (baud, flags)
        self.settle = 0.0            # s after UART_OPEN during which writes get ERR_INTERLOCK (VTref settling)
        self.opened_at = {}
        self.lock = threading.Lock()
        self.stop = False
        threading.Thread(target=self._run, daemon=True).start()

    def restart(self):
        self.t0 = time.monotonic()
        self.boot_id += 1
        self.uart.clear()

    def cmds(self, cmd):
        with self.lock:
            return [a for c, a in self.log if c == cmd]

    def _send(self, pl):
        os.write(self.master, P.frame_encode(pl))

    def _run(self):
        buf = bytearray()
        while not self.stop:
            try:
                d = os.read(self.master, 65536)
            except OSError:          # the daemon has not opened the port yet, or closed it
                time.sleep(0.01)
                continue
            buf += d
            while 0 in buf:
                i = buf.index(0)
                raw, buf[:] = bytes(buf[:i]), buf[i + 1:]
                pl = P.frame_decode(raw) if raw else None
                if pl and len(pl) >= 2:
                    self._handle(pl[0], pl[1], bytes(pl[2:]))

    def _handle(self, seq, cmd, a):
        with self.lock:
            self.log.append((cmd, a))
            if cmd in self.drop:
                self.drop.discard(cmd)
                return
        body = b""
        if cmd == P.PING:
            body = a
        elif cmd == P.INFO:
            body = struct.pack("<IIII", 5, int((time.monotonic() - self.t0) * 1000), 1, 100000)
            body += b"fake 0.5.0".ljust(32, b"\0") + struct.pack("<I", self.boot_id)
        elif cmd == P.CH_STATE:
            body = struct.pack("<iBBBBIIBBBB", 3300, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0)
        elif cmd == P.SWD_ATTACH:
            body = struct.pack("<IBBH", 0x2BA01477, 0, 0, 8000) + b"fake part".ljust(32, b"\0")
        elif cmd == P.UART_OPEN:
            self.uart[a[0]] = struct.unpack_from("<II", a, 1)
            self.opened_at[a[0]] = time.monotonic()
            body = struct.pack("<IB", self.uart[a[0]][0], 2)
        elif cmd == P.UART_WRITE:
            if a[0] not in self.uart:
                self._send(bytes([seq, cmd | 0x80, P.ERR_STATE & 0xFF, 0, 0]))
                return
            if time.monotonic() - self.opened_at[a[0]] < self.settle:
                self._send(bytes([seq, cmd | 0x80, P.ERR_INTERLOCK & 0xFF, 0, 0]))
                return
            body = struct.pack("<H", len(a) - 1)
            self._send(bytes([seq, cmd | 0x80, 0]) + body)
            self._send(bytes([0, P.EVT_UART_RX, 0, a[0], 0]) + struct.pack("<I", 0) + a[1:])
            return
        elif cmd == P.UART_CLOSE:
            self.uart.pop(a[0], None)
        elif cmd == P.MEM_READ:
            body = bytes(4 * struct.unpack_from("<H", a, 5)[0])
        self._send(bytes([seq, cmd | 0x80, 0]) + body)


class DaemonTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        os.environ["XDG_RUNTIME_DIR"] = self.tmp.name
        self.board = FakeBoard()
        self.lines = []
        self.d = server.Daemon(self.board.path, log=self.lines.append, board="FAKE0001")
        threading.Thread(target=self.d.serve, daemon=True).start()
        for _ in range(100):
            if self.d.ser is not None and self.d.info and os.path.exists(self.d.sock_path):
                break
            time.sleep(0.02)
        self.assertTrue(self.d.info, "daemon did not connect to the simulated board: %s" % self.lines)
        self.sock = "unix:" + self.d.sock_path

    def tearDown(self):
        self.d.stop.set()
        time.sleep(0.4)
        self.board.stop = True
        self.tmp.cleanup()

    def test_two_clients_share_the_board(self):
        with Esw(self.sock) as c1, Esw(self.sock) as c2:
            self.assertEqual(c1.info()["fw"], "fake 0.5.0")
            out = {}

            def pings(c, tag):
                out[tag] = [c.ping(b"%s%d" % (tag, i)) for i in range(50)]

            ths = [threading.Thread(target=pings, args=(c, t)) for c, t in ((c1, b"x"), (c2, b"y"))]
            for t in ths:
                t.start()
            for t in ths:
                t.join()
            self.assertEqual(out[b"x"], [b"x%d" % i for i in range(50)])   # each answer to its own client
            self.assertEqual(out[b"y"], [b"y%d" % i for i in range(50)])

    def test_channel_ownership_and_cleanup(self):
        c1 = Esw(self.sock)
        c1.hold(0, P.GROUP_SWD)
        c1.attach(0)
        with Esw(self.sock) as c2:
            with self.assertRaises(EswError) as e:
                c2.attach(0)
            self.assertIn("in use by another program", str(e.exception))
            c2.hold(1, P.GROUP_SWD)          # the other channel is free
            c2.release(1, P.GROUP_SWD)
            c2.mode(1, "uart", "off")        # switch modes are always allowed
            c2.mode(1, "uart", "auto")
        c1.close()                            # gone without detach/release: the daemon cleans up
        for _ in range(100):
            if bytes([0, P.GROUP_SWD]) in self.board.cmds(P.CH_RELEASE):
                break
            time.sleep(0.02)
        self.assertIn(bytes([0]), self.board.cmds(P.SWD_DETACH))
        self.assertIn(bytes([0, P.GROUP_SWD]), self.board.cmds(P.CH_RELEASE))

    def test_tty_bridge_follows_the_terminal(self):
        tty = self.d.ttys[0].path
        s = serial.Serial(tty, 1000000, timeout=2)
        data = bytes(range(256)) * 64
        time.sleep(0.2)
        s.write(data)
        got = s.read(len(data))
        self.assertEqual(got, data)
        self.assertEqual(self.board.uart[0][0], 1000000)   # the terminal's baud rate reached the board
        with Esw(self.sock) as c:                           # the bridge owns the UART
            with self.assertRaises(EswError):
                c.uart_open(0, 115200)
        s.close()
        for _ in range(100):
            if 0 not in self.board.uart:
                break
            time.sleep(0.02)
        self.assertNotIn(0, self.board.uart)                # TTY closed: UART closed (switches may open)

    def test_board_restart_with_a_lost_frame(self):
        with Esw(self.sock) as c:
            c.daemon(server.OP_TTY_POLICY, bytes([1, 0]))   # free channel B's UART for this client
            self.board.restart()
            self.board.drop.add(P.INFO)                     # the next probe goes to the restarting board
            t0 = time.monotonic()
            while not any("board restarted" in s for s in self.lines) and time.monotonic() - t0 < 5:
                time.sleep(0.05)
            self.assertTrue(any("board restarted" in s for s in self.lines), self.lines)
            self.assertLess(time.monotonic() - t0, 3.5)     # found by order, not by the 5 s expiry
            t1 = time.monotonic()
            c.uart_close(1)                                 # a barrier command: must not wait for the lost frame
            self.assertLess(time.monotonic() - t1, 1.0)

    def test_suspend_and_resume(self):
        with Esw(self.sock) as c:
            c.daemon(server.OP_SUSPEND)
            st, body = c.request(P.PING, b"x", check=False)
            self.assertEqual(st, -7)
            self.assertIn(b"suspended", body)
            st, _ = c.request(P.DAEMON, bytes([server.OP_RESUME]), timeout=20, check=False)
            self.assertEqual(st, 0)
            self.assertEqual(c.ping(b"back"), b"back")


    # ---- regressions from the 2026-11-11 review ----------------------------------------------------------------
    def wait_for(self, cond, timeout=3.0):
        t0 = time.monotonic()
        while not cond() and time.monotonic() - t0 < timeout:
            time.sleep(0.02)
        return cond()

    def test_first_bytes_survive_vtref_settling(self):
        self.board.settle = 0.15   # the real interlock refuses writes until the switches closed
        s = serial.Serial(self.d.ttys[0].path, 115200, timeout=2)
        s.write(b"first bytes")    # right away, no sleep
        self.assertEqual(s.read(11), b"first bytes")
        s.close()

    def test_echo_into_a_closed_tty_still_goes_out(self):
        fd = os.open(self.d.ttys[1].path, os.O_RDWR | os.O_NOCTTY)
        os.write(fd, b"reset\n")
        os.close(fd)               # `echo reset > ttyB`: open, write, close at once
        self.assertTrue(self.wait_for(lambda: any(a[1:] == b"reset\n" for a in self.board.cmds(P.UART_WRITE))))
        self.assertTrue(self.wait_for(lambda: 1 not in self.board.uart))   # drained, then closed again

    def test_bridge_reopens_a_uart_the_board_closed(self):
        s = serial.Serial(self.d.ttys[0].path, 115200, timeout=2)
        time.sleep(0.2)
        self.board.uart.clear()    # host loss or restart on the board: the stream is gone
        s.write(b"hello")
        self.assertEqual(s.read(5), b"hello")   # reopened instead of writing into nothing
        self.assertGreaterEqual(len(self.board.cmds(P.UART_OPEN)), 2)
        self.assertLess(len(self.board.cmds(P.UART_WRITE)), 50)   # no flood of refused writes
        s.close()

    def test_lost_channel_frame_does_not_stall_uart_open(self):
        with Esw(self.sock, timeout=0.5) as c:
            self.board.drop.add(P.SWD_STATS)
            with self.assertRaises(TimeoutError):
                c.stats(0)         # its answer never comes: the frame stays in flight on the daemon
            t0 = time.monotonic()
            s = serial.Serial(self.d.ttys[0].path, 115200, timeout=2)   # needs a UART_OPEN (blocks the dispatcher)
            s.write(b"x")
            self.assertEqual(s.read(1), b"x")
            self.assertLess(time.monotonic() - t0, 1.5)
            s.close()

    def test_one_uart_owner_for_stream_and_hold(self):
        with Esw(self.sock) as c1, Esw(self.sock) as c2:
            c1.daemon(server.OP_TTY_POLICY, bytes([1, 0]))
            c1.uart_open(1, 115200)
            for call in (lambda: c2.hold(1, P.GROUP_UART), lambda: c2.release(1, P.GROUP_UART),
                         lambda: c2.uart_write(1, b"x")):
                with self.assertRaises(EswError):
                    call()
            st, body = c2.request(P.DAEMON, bytes([server.OP_TTY_POLICY, 1, 1]), check=False)
            self.assertEqual(st, P.ERR_STATE)   # the bridge cannot take a UART a program uses
            c1.uart_close(1)
            c2.daemon(server.OP_TTY_POLICY, bytes([1, 1]))

    def test_oversize_frame_refused(self):
        with Esw(self.sock) as c:
            st, body = c.request(P.PING, b"x" * 3000, check=False)
            self.assertEqual(st, -1)
            self.assertEqual(c.ping(b"ok"), b"ok")


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: MIT
"""esp-swd host daemon (`esw serve`): owns the board's USB port and

  * bridges each DUT UART to a pseudo-terminal, so standard tools (picocom, minicom, screen, pyserial, ...) use
    them like any serial port: <run dir>/<board>/ttyA and ttyB;
  * lets any number of programs share the board through a local socket (<run dir>/<board>/ctl.sock). Esw() finds
    it by itself, so flashing scripts and the CLI run while the TTYs stay open;
  * keeps the board's host lease alive and reconnects after a board reset or replug.

Each program speaks the normal framed protocol over the socket. The daemon renumbers requests, keeps the board's
receive budget for all of them together (its USB buffer has no flow control), and gives every channel one owner
at a time: the program that holds/attaches a channel's SWD, and for the UART the TTY bridge (unless its policy is
off) or the program that opened it. When a program disconnects, the daemon ends what it left behind (detach,
release, UART close), as the board itself would on host loss.

UART bridge policy per channel: "open" (default): the UART runs and its switches may close while some program has
the TTY open; "always": from start (output that arrives while no program has the TTY open is kept, 64 KB, and
delivered on open); "off": no bridge, the channel's UART is free for scripts. The baud rate comes from the TTY
(what the terminal program sets, any rate up to 5 Mbaud); parity and stop bits from --format (Linux PTYs cannot
carry parity). The isolation switches still follow the interlock: no VTref, no connection; writes are then dropped.
"""
import collections
import fcntl
import json
import os
import select
import signal
import socket
import struct
import termios
import threading
import time
import tty as ttymod

import serial

from . import link
from . import proto as P

# daemon requests (P.DAEMON, op u8, ...)
OP_STATUS, OP_SUSPEND, OP_RESUME, OP_TTY_POLICY, OP_QUIT = 0, 1, 2, 3, 4
POLICIES = ("off", "open", "always")

DEV_BUDGET = 7168            # unanswered bytes for device commands (the board's 8 KB USB RX ring)
CH_BUDGET = P.CH_QUEUE * 2310
BARRIER_HOLD = 1.5          # s: while a command that blocks the board's dispatcher (UART_OPEN/CLOSE, REBOOT, UART mode)
                             # is unanswered, nothing else is sent, for at most this long (it blocks for <= ~1.2 s)
MAX_PAYLOAD = 2304           # ESW_PROTO_MAX_PAYLOAD: the board drops longer frames
SUSPEND_MAX = 120.0          # s: a suspend (firmware update) that nobody resumes ends by itself
INTERLOCK_GRACE = 1.0        # s after a UART open: writes refused by the interlock are retried (VTref settling)
EXPIRE_S = 180.0             # a request the board never answered gives its credit back after this ...
EXPIRE_QUICK = 5.0           # ... or after this for commands the board answers at once
QUICK = (P.PING, P.INFO, P.REBOOT, P.CH_STATE, P.UART_OPEN, P.UART_CLOSE, P.UART_WRITE, P.UART_STATS)
UART_CHUNK = 2048
TX_HIGH = 256 * 1024         # bytes from a TTY waiting for the UART: stop reading the PTY above this
RX_MAX = 4 << 20             # bytes for a TTY its program does not read: oldest dropped above this
BACKLOG = 64 * 1024

TCGETS2 = 0x802C542A


def _frames(buf, data):
    """Feed bytes; yields decoded payloads (None for a bad frame)."""
    buf += data
    while True:
        i = buf.find(0)
        if i < 0:
            if len(buf) > 65536:
                buf.clear()
            return
        raw = bytes(buf[:i])
        del buf[:i + 1]
        if raw:
            yield P.frame_decode(raw)


def _key(pl):
    """Budget key: the channel for commands the board queues to that channel's worker, else "dev" (answered in
    order by the dispatcher). CH_MODE on the UART group runs on the dispatcher."""
    cmd = pl[1]
    if cmd == P.CH_MODE and len(pl) > 3 and pl[3] == P.GROUP_UART:
        return "dev"
    if len(pl) > 2 and pl[2] < 2 and (cmd in (P.CH_HOLD, P.CH_RELEASE, P.CH_MODE) or 0x20 <= cmd <= 0x2F):
        return pl[2]
    return "dev"


def _blocks_dispatcher(payload):
    cmd = payload[0]
    return cmd in (P.UART_OPEN, P.UART_CLOSE, P.REBOOT) or (cmd == P.CH_MODE and len(payload) > 2 and
                                                             payload[2] == P.GROUP_UART)


class Request:
    __slots__ = ("payload", "cmd", "key", "n", "owner", "cseq", "cb", "t", "order", "barrier")

    def __init__(self, payload, owner=None, cb=None):
        self.payload = payload        # without seq: cmd, args...
        self.cmd = payload[0]
        self.key = _key(b"\0" + payload)
        self.n = len(payload) + len(payload) // 254 + 8
        self.owner, self.cseq, self.cb, self.t, self.order = owner, 0, cb, 0.0, 0
        self.barrier = _blocks_dispatcher(payload)


class Client:
    def __init__(self, daemon, sock, n):
        self.d, self.sock, self.name = daemon, sock, "client%d" % n
        self.out = collections.deque()
        self.out_bytes = 0
        self.active = False   # sent a frame (probe connections that only test the socket do not count)
        self.cv = threading.Condition()
        self.alive = True
        threading.Thread(target=self._writer, name=self.name + "-tx", daemon=True).start()
        threading.Thread(target=self._reader, name=self.name + "-rx", daemon=True).start()

    def send(self, payload):
        fr = P.frame_encode(payload)
        with self.cv:
            if not self.alive:
                return
            if self.out_bytes > (64 << 20):   # not reading at all: let it go
                self.alive = False
                self.cv.notify_all()
                return
            self.out.append(fr)
            self.out_bytes += len(fr)
            self.cv.notify_all()

    def respond(self, cseq, cmd, status, body=b""):
        self.send(bytes([cseq, cmd | 0x80, status & 0xFF]) + body)

    def _writer(self):
        try:
            while True:
                with self.cv:
                    while self.alive and not self.out:
                        self.cv.wait()
                    if not self.alive:
                        return
                    fr = self.out.popleft()
                    self.out_bytes -= len(fr)
                self.sock.sendall(fr)
        except OSError:
            pass
        finally:
            self.close()

    def _reader(self):
        buf = bytearray()
        try:
            while self.alive:
                d = self.sock.recv(65536)
                if not d:
                    break
                for pl in _frames(buf, d):
                    if pl and len(pl) >= 2:
                        self.active = True
                        self.d.from_client(self, pl)
        except OSError:
            pass
        finally:
            self.close()

    def close(self):
        with self.cv:
            self.alive = False
            first, self.closed = not getattr(self, "closed", False), True
            self.cv.notify_all()
        if not first:
            return
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        self.d.client_gone(self)


class Tty:
    """One channel's PTY bridge. State is guarded by the daemon lock."""

    def __init__(self, d, ch, policy, fmt, path):
        self.d, self.ch, self.policy = d, ch, policy
        self.parity, self.stop2 = fmt
        self.master, slave = os.openpty()
        self.slave_name = os.ttyname(slave)
        ttymod.setraw(self.master)   # the slave side's settings: raw 8-bit, no echo (like a fresh USB serial port)
        a = termios.tcgetattr(self.master)
        a[4] = a[5] = termios.B115200
        termios.tcsetattr(self.master, termios.TCSANOW, a)
        os.close(slave)
        fl = fcntl.fcntl(self.master, fcntl.F_GETFL)
        fcntl.fcntl(self.master, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.path = path
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        os.symlink(self.slave_name, path)
        self.is_open = False
        self.uart_on = False       # UART_OPEN answered ok, settings = self.cur
        self.cur = None
        self.busy = False          # an open/close of ours is in flight
        self.tx = bytearray()
        self.tx_inflight = False
        self.rx = bytearray()
        self.backlog = bytearray()
        self.stats = dict(rx=0, tx=0, rx_dropped=0, tx_dropped=0, lost_flags=0, opens=0)
        self.retry_at = 0.0
        self.tx_at = 0.0           # board TX buffer was full: next UART_WRITE not before this
        self.txgen = 0             # bumped whenever tx is dropped: a late write ack must not cut new data
        self.opened_at = 0.0       # when the UART stream was (re)opened
        self.note = ""

    def settings(self):
        """(baud, parity, stop2) the program on the TTY asked for."""
        try:
            # struct termios2: c_ospeed holds the rate for standard (Bxxx) and custom (BOTHER) settings alike.
            # (Python 3.14's termios maps the Bxxx constants to plain numbers, so no table of them is used.)
            raw = fcntl.ioctl(self.master, TCGETS2, bytes(44))
            cflag, baud = struct.unpack_from("<I", raw, 8)[0], struct.unpack_from("<I", raw, 40)[0]
            stop2 = bool(cflag & termios.CSTOPB) or self.stop2
        except OSError:
            baud, stop2 = 115200, self.stop2
        return (max(300, min(baud or 115200, 5000000)), self.parity, stop2)

    def poll_open(self):
        p = select.poll()
        p.register(self.master, select.POLLIN)
        ev = p.poll(0)
        return not (ev and ev[0][1] & select.POLLHUP)

    def want(self):
        # "open": while a program has the TTY open, and after it closed until what it wrote went out (like a
        # serial port that drains on close: `echo cmd > ttyA` works)
        return self.policy == "always" or (self.policy == "open" and (self.is_open or bool(self.tx) or
                                                                      self.tx_inflight))

    def deliver(self, data, flags):
        """RX event from the board (reader thread, lock held)."""
        if flags & P.UART_RX_LOST:
            self.stats["lost_flags"] += 1
        if not self.is_open:
            if self.policy == "always":
                self.backlog += data
                if len(self.backlog) > BACKLOG:
                    cut = len(self.backlog) - BACKLOG
                    del self.backlog[:cut]
                    self.stats["rx_dropped"] += cut
            else:
                self.stats["rx_dropped"] += len(data)
            return
        self.stats["rx"] += len(data)
        if not self.rx:
            try:
                n = os.write(self.master, data)
                data = data[n:]
            except BlockingIOError:
                pass
            except OSError:
                self.stats["rx_dropped"] += len(data)
                return
        if data:
            self.rx += data
            if len(self.rx) > RX_MAX:
                cut = len(self.rx) - RX_MAX
                del self.rx[:cut]
                self.stats["rx_dropped"] += cut
            self.d.wake()

    def close(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass
        os.close(self.master)


class Daemon:
    def __init__(self, port=None, policies=("open", "open"), fmts=(("N", False), ("N", False)), log=print,
                 keepalive=1.0, loopback=False, board=None):
        """board: test hook; with it, port is used as given (e.g. a simulated board on a PTY)."""
        if board:
            self.dev, self.board, self.fixed_dev = port, board, True
        else:
            kind, where = link.resolve(port)
            if kind == "unix":
                raise RuntimeError("a daemon already serves this board (%s)" % where)
            self.dev, self.fixed_dev = where, False
            self.board = link.usb_serial(where)
            if not self.board:
                raise RuntimeError("%s: not a USB port (the daemon follows the board by its USB serial number)"
                                   % where)
        self.log = log
        self.keepalive = keepalive
        self.loopback = loopback   # self-test: the bridge opens the UARTs in internal loopback (no DUT, no pads)
        self.lock = threading.RLock()
        self.txlock = threading.Lock()   # taken under self.lock, released after the write: frames leave in order
        self.ser = None
        self.suspended = False
        self.stop = threading.Event()
        self.seq = 0
        self.inflight = {}                                  # board seq -> Request
        self.order = 0                                      # send order of requests
        self.used = {"dev": 0, 0: 0, 1: 0}                  # unanswered bytes per budget key
        self.frames = {"dev": 0, 0: 0, 1: 0}
        self.queue = {"dev": collections.deque(), 0: collections.deque(), 1: collections.deque()}
        self.barrier = False
        self.last_tx = 0.0
        self.last_info = 0.0
        self.pause_until = 0.0   # the board is restarting (REBOOT answered): frames sent now would be lost
        self.suspend_until = 0.0
        self.barrier = None      # the unanswered request that blocks the board's dispatcher
        self.clients = set()
        self.nclients = 0
        self.swd_owner = [None, None]                       # client holding/attached to the channel's SWD
        self.swd_attached = [False, False]
        self.uart_owner = [None, None]                      # client using the channel's UART (bridge off) ...
        self.uart_open = [False, False]                     # ... with its stream open
        self.uart_held = [False, False]                     # ... holding the UART group directly
        self.info = {}
        self.dir = link.board_dir(self.board)
        os.makedirs(self.dir, mode=0o700, exist_ok=True)
        self.sock_path = link.daemon_socket(self.board)
        self.wr, self.ww = os.pipe()
        os.set_blocking(self.wr, False)
        os.set_blocking(self.ww, False)
        self.ttys = [Tty(self, ch, policies[ch], fmts[ch], os.path.join(self.dir, "tty" + "AB"[ch]))
                     for ch in (0, 1)]

    # ---- board link ------------------------------------------------------------------------------------------
    def _device(self):
        if self.fixed_dev:
            return self.dev if os.path.exists(self.dev) else None
        if os.path.exists(self.dev) and link.usb_serial(self.dev) == self.board:
            return self.dev
        return link.find_boards().get(self.board)

    def _connect(self):
        dev = self._device()
        if not dev:
            return False
        try:
            ser = serial.Serial(dev, 115200, timeout=0.05, exclusive=True)
        except (serial.SerialException, OSError):
            return False
        ser.reset_input_buffer()
        with self.lock:
            self.ser = ser
            self.dev = dev
            self.info = {}
            for t in self.ttys:
                t.uart_on, t.cur, t.busy, t.tx_inflight = False, None, False, False
        self.log("board %s connected on %s" % (self.board, dev))
        self._probe()
        return True

    def _probe(self):
        """INFO once a second: keeps the board's host lease, and a falling uptime tells that the board restarted
        (reboot, crash, watchdog). USB-Serial-JTAG stays enumerated across a restart, so the port alone never
        shows it."""
        self.last_info = time.monotonic()
        r = Request(bytes([P.INFO]))
        r.cb = lambda st, body: self._got_info(st, body, r)
        self._enqueue(r)

    def _got_info(self, st, body, req):
        if st != 0 or len(body) < 48:
            return
        proto, up, rr, heap = struct.unpack_from("<IIII", body)
        boot = struct.unpack_from("<I", body, 48)[0] if len(body) >= 52 else None
        new = dict(proto=proto, fw=body[16:48].split(b"\0")[0].decode(), uptime_ms=up, reset_reason=rr, boot_id=boot)
        with self.lock:
            prev, self.info = self.info, new
            prev_t, self.info_t = getattr(self, "info_t", req.t), req.t
        if prev:
            if boot is not None and prev.get("boot_id") is not None:
                restarted = boot != prev["boot_id"]   # firmware 0.5.0+: a new random id at every boot
            else:   # older firmware: uptime must advance with the wall clock (misses a restart right after a probe)
                restarted = up + 300 < prev["uptime_ms"] + (req.t - prev_t) * 1000
            if restarted:
                self._board_restarted(req, rr)
        if not prev or prev["fw"] != new["fw"]:
            self.log("firmware: %s (protocol %d)" % (new["fw"], proto))
            if proto < 5:
                self.log("warning: firmware protocol %d is older than this host (%d)" % (proto, P.PROTO_VERSION))

    def _board_restarted(self, probe, reason):
        """Requests sent before the probe that saw the restart and still unanswered went to the old board, or to
        the new one before its USB driver was up: fail them now and give their credit back (a queued UART open
        would otherwise wait for them). Sessions and UART streams are gone."""
        with self.lock:
            lost = [(s, r) for s, r in self.inflight.items() if r.order < probe.order]
            for s, r in lost:
                del self.inflight[s]
                self._credit(r)
            self.swd_attached = [False, False]
            for t in self.ttys:
                if not t.busy:
                    t.uart_on, t.cur = False, None
        self.log("board restarted (reset reason %d): %d request(s) lost" % (reason, len(lost)))
        for s, r in lost:
            self._finish(r, -7, b"board restarted")
        self._pump()

    def _link_lost(self, why, failed=None):
        with self.lock:
            if failed is not None and self.ser is not failed:
                return   # an error from a link that was already replaced
            ser, self.ser = self.ser, None
            lost = list(self.inflight.values())
            self.inflight.clear()
            for k in self.used:
                self.used[k] = self.frames[k] = 0
            queued = [r for q in self.queue.values() for r in q]
            for q in self.queue.values():
                q.clear()
            self.barrier = None
            self.swd_attached = [False, False]
            for t in self.ttys:
                t.uart_on, t.cur, t.busy, t.tx_inflight = False, None, False, False
        if ser:
            try:
                ser.close()
            except OSError:
                pass
            self.log("board link lost: %s" % why)
        for r in lost + queued:
            self._finish(r, -7, b"board link lost")

    def _reader(self):
        buf = bytearray()
        while not self.stop.is_set():
            ser = self.ser
            if ser is None:
                if self.suspended and time.monotonic() > self.suspend_until:
                    self.suspended = False
                    self.log("suspend not resumed within %.0f s: reconnecting" % SUSPEND_MAX)
                if not self.suspended and self._connect():
                    continue
                time.sleep(0.3)
                continue
            try:
                d = ser.read(ser.in_waiting or 1)
            except (serial.SerialException, OSError, TypeError) as e:
                self._link_lost(e, ser)
                buf.clear()
                continue
            if d:
                for pl in _frames(buf, d):
                    if pl and len(pl) >= 3:
                        self._from_board(pl)
            self._expire()

    def _from_board(self, pl):
        cmd = pl[1]
        if P.EVT_FIRST <= cmd <= P.EVT_LAST:
            if cmd == P.EVT_UART_RX and len(pl) >= 9 and pl[3] < 2:
                ch = pl[3]
                with self.lock:
                    t = self.ttys[ch]
                    if t.policy != "off":
                        t.deliver(bytes(pl[9:]), pl[4])
                        return
                    owner = self.uart_owner[ch]
                if owner:
                    owner.send(bytes(pl))
            return
        with self.lock:
            r = self.inflight.get(pl[0])
            if r is None or (cmd & 0x7F) != r.cmd or not cmd & 0x80:
                return
            del self.inflight[pl[0]]
            self._credit(r)
            # the dispatcher answers device commands strictly in order, and each channel's worker its own: an older
            # request of the same key still open was lost (board restart, or a frame the board dropped)
            skipped = [(s, x) for s, x in self.inflight.items() if x.key == r.key and x.order < r.order]
            for s, x in skipped:
                del self.inflight[s]
                self._credit(x)
        for s, x in skipped:
            self.log("cmd 0x%02X (%s) was lost: a later one got its answer" % (
                x.cmd, x.owner.name if x.owner else "daemon"))
            self._finish(x, -7, b"request lost (board restart or link error)")
        self._finish(r, struct.unpack("b", pl[2:3])[0], bytes(pl[3:]))
        self._pump()

    def _credit(self, r):   # lock held
        self.used[r.key] -= r.n
        self.frames[r.key] -= 1
        if self.barrier is r:
            self.barrier = None

    def _finish(self, r, st, body):
        if r.owner is not None:
            self._after(r, st, body)
            r.owner.respond(r.cseq, r.cmd, st, body)
        elif r.cb:
            try:
                r.cb(st, body)
            except Exception as e:  # noqa: BLE001
                self.log("internal callback failed: %r" % e)

    def _expire(self):
        now, old = time.monotonic(), []
        with self.lock:
            for s, r in list(self.inflight.items()):
                if now - r.t > (EXPIRE_QUICK if r.cmd in QUICK else EXPIRE_S):
                    del self.inflight[s]
                    self._credit(r)
                    old.append(r)
        for r in old:
            self.log("no answer to cmd 0x%02X (%s) after %.1f s" % (r.cmd, r.owner.name if r.owner else "daemon",
                                                                  now - r.t))
            self._finish(r, -7, b"no answer from the board")
        if old:
            self._pump()

    def submit(self, payload, owner=None, cseq=0, cb=None):
        r = Request(bytes(payload), owner, cb)
        r.cseq = cseq
        self._enqueue(r)

    def _enqueue(self, r):
        with self.lock:
            if self.ser is None:
                fail = True
            else:
                fail = False
                self.queue[r.key].append(r)
        if fail:
            self._finish(r, -7, b"board not connected" if not self.suspended else b"daemon suspended")
            return
        self._pump()

    def _limits(self, key):
        return (DEV_BUDGET, 64) if key == "dev" else (CH_BUDGET, P.CH_QUEUE)

    def _pump(self):
        out = []
        with self.lock:
            ser = self.ser
            if ser is None or time.monotonic() < self.pause_until:
                return
            now = time.monotonic()
            for key, q in self.queue.items():
                while q:
                    # while a command that blocks the board's dispatcher is unanswered, send nothing: the board's
                    # 8 KB USB buffer would have to hold it. Bounded, so a lost answer cannot starve the host lease
                    if self.barrier is not None and now - self.barrier.t < BARRIER_HOLD:
                        break
                    r = q[0]
                    nb, nf = self._limits(key)
                    if self.frames[key] and (self.used[key] + r.n > nb or self.frames[key] >= nf):
                        break
                    q.popleft()
                    seq = self._next_seq()
                    r.t = time.monotonic()
                    self.order += 1
                    r.order = self.order
                    self.inflight[seq] = r
                    self.used[key] += r.n
                    self.frames[key] += 1
                    if r.barrier:
                        self.barrier = r
                    out.append(P.frame_encode(bytes([seq]) + r.payload))
            if not out:
                return
            self.txlock.acquire()
        try:
            ser.write(b"".join(out))
            self.last_tx = time.monotonic()
            err = None
        except (serial.SerialException, OSError, TypeError) as e:
            err = e
        finally:
            self.txlock.release()
        if err:
            self._link_lost(err, ser)

    def _next_seq(self):   # lock held
        for _ in range(256):
            self.seq = (self.seq + 1) & 0xFF
            if self.seq and self.seq not in self.inflight:
                return self.seq
        raise RuntimeError("no free sequence number")

    # ---- clients --------------------------------------------------------------------------------------------
    def from_client(self, c, pl):
        cseq, cmd, data = pl[0], pl[1], bytes(pl[2:])
        if cmd == P.DAEMON:
            self._daemon_cmd(c, cseq, data)
            return
        if len(pl) > MAX_PAYLOAD:
            c.respond(cseq, cmd, -1, b"frame longer than the board accepts")
            return
        r = Request(bytes([cmd]) + data, c)
        r.cseq = cseq
        with self.lock:   # ownership and queue order decided together: a cleanup cannot slip in between
            why = self._refuse(c, cmd, data)
            queued = not why and self.ser is not None
            if queued:
                self.queue[r.key].append(r)
        if why:
            c.respond(cseq, cmd, P.ERR_STATE, why.encode())
        elif not queued:
            self._finish(r, -7, b"board not connected" if not self.suspended else b"daemon suspended")
        else:
            self._pump()

    def _refuse(self, c, cmd, data):
        """Ownership rules; returns a reason to refuse, or None. Also takes ownership. Lock held."""
        ch = data[0] if data else None
        if not c.alive:
            return "connection closing"
        with self.lock:
            if ch is not None and ch < 2:
                if (cmd in (P.CH_HOLD, P.CH_RELEASE) and len(data) > 1 and data[1] == P.GROUP_SWD) or \
                        0x20 <= cmd <= 0x2F:
                    o = self.swd_owner[ch]
                    if o is not None and o is not c and o.alive:
                        return "channel %s SWD is in use by another program" % "AB"[ch]
                    if cmd in (P.CH_HOLD, P.SWD_ATTACH):
                        self.swd_owner[ch] = c
                elif cmd in (P.UART_OPEN, P.UART_CLOSE, P.UART_WRITE, P.UART_STATS) or \
                        (cmd in (P.CH_HOLD, P.CH_RELEASE) and len(data) > 1 and data[1] == P.GROUP_UART):
                    if cmd == P.UART_STATS:
                        return None
                    t = self.ttys[ch]
                    if t.policy != "off" or t.uart_on or t.busy:
                        return ("channel %s UART belongs to the TTY bridge (%s); `esw tty %s off` frees it"
                                % ("AB"[ch], t.path, "AB"[ch]))
                    # one owner for the stream and the group hold: the board keeps both in one holder bit
                    o = self.uart_owner[ch]
                    if o is not None and o is not c and o.alive:
                        return "channel %s UART is in use by another program" % "AB"[ch]
                    if cmd == P.UART_OPEN:
                        self.uart_owner[ch], self.uart_open[ch] = c, True
                    elif cmd == P.CH_HOLD:
                        self.uart_owner[ch], self.uart_held[ch] = c, True
        return None

    def _after(self, r, st, body):
        """Ownership bookkeeping once the board answered a client's request."""
        p = r.payload
        if r.cmd == P.REBOOT and st == 0:
            with self.lock:
                self.pause_until = time.monotonic() + 1.2
            return
        ch = p[1] if len(p) > 1 else None
        if ch is None or ch > 1:
            return
        with self.lock:
            if r.cmd == P.SWD_ATTACH:
                self.swd_attached[ch] = st == 0
            elif r.cmd == P.TGT_UNLOCK and st == 0:
                self.swd_attached[ch] = True
            elif r.cmd == P.SWD_DETACH:
                self.swd_attached[ch] = False
            elif r.cmd == P.CH_RELEASE and len(p) > 2 and p[2] == P.GROUP_SWD and self.swd_owner[ch] is r.owner:
                self.swd_owner[ch] = None
                self.swd_attached[ch] = False
            elif self.uart_owner[ch] is r.owner and (
                    (r.cmd == P.CH_RELEASE and len(p) > 2 and p[2] == P.GROUP_UART) or r.cmd == P.UART_CLOSE or
                    (r.cmd == P.UART_OPEN and st != 0)):
                # the board's UART_CLOSE and a failed UART_OPEN release the group hold too
                self.uart_held[ch] = False
                if r.cmd != P.CH_RELEASE:
                    self.uart_open[ch] = False
                if not self.uart_open[ch] and not self.uart_held[ch]:
                    self.uart_owner[ch] = None
            elif r.cmd == P.CH_MODE and len(p) > 3 and p[2] == P.GROUP_SWD and p[3] == P.MODES["off"]:
                self.swd_attached[ch] = False

    def client_gone(self, c):
        """End what a disconnected program left behind, like the board does on host loss."""
        todo = []
        with self.lock:
            self.clients.discard(c)
            for ch in (0, 1):
                if self.swd_owner[ch] is c:
                    self.swd_owner[ch] = None
                    if self.swd_attached[ch]:
                        todo.append(bytes([P.SWD_DETACH, ch]))
                    self.swd_attached[ch] = False
                    todo.append(bytes([P.CH_RELEASE, ch, P.GROUP_SWD]))
                if self.uart_owner[ch] is c:
                    if self.uart_open[ch]:
                        todo.append(bytes([P.UART_CLOSE, ch]))   # also releases the group hold
                    elif self.uart_held[ch]:
                        todo.append(bytes([P.CH_RELEASE, ch, P.GROUP_UART]))
                    self.uart_owner[ch], self.uart_open[ch], self.uart_held[ch] = None, False, False
        if todo:
            self.log("%s gone: %s" % (c.name, ", ".join("%02X/%s" % (t[0], "AB"[t[1]]) for t in todo)))
        for t in todo:
            self.submit(t)

    def _daemon_cmd(self, c, cseq, data):
        op = data[0] if data else OP_STATUS
        if op == OP_STATUS:
            c.respond(cseq, P.DAEMON, 0, json.dumps(self.status()).encode())
        elif op == OP_SUSPEND:
            self.suspended = True
            self.suspend_until = time.monotonic() + SUSPEND_MAX
            c.respond(cseq, P.DAEMON, 0)
            time.sleep(0.05)
            self._link_lost("suspended for a firmware update")
        elif op == OP_RESUME:
            self.suspended = False
            t0 = time.monotonic()
            while self.ser is None and time.monotonic() - t0 < 15:
                time.sleep(0.1)
            c.respond(cseq, P.DAEMON, 0 if self.ser else -7, b"" if self.ser else b"board did not come back")
        elif op == OP_TTY_POLICY and len(data) >= 3 and data[1] < 2 and data[2] < len(POLICIES):
            with self.lock:
                o = self.uart_owner[data[1]]
                if POLICIES[data[2]] != "off" and o is not None and o.alive:
                    busy = o.name
                else:
                    busy = None
                    self.ttys[data[1]].policy = POLICIES[data[2]]
            if busy:
                c.respond(cseq, P.DAEMON, P.ERR_STATE,
                          ("channel %s UART is in use by %s" % ("AB"[data[1]], busy)).encode())
                return
            self.log("TTY %s policy: %s" % ("AB"[data[1]], POLICIES[data[2]]))
            self.wake()
            c.respond(cseq, P.DAEMON, 0)
        elif op == OP_QUIT:
            self.log("stop requested by %s" % c.name)
            c.respond(cseq, P.DAEMON, 0)
            self.stop.set()
        else:
            c.respond(cseq, P.DAEMON, -1)

    def status(self):
        with self.lock:
            return dict(board=self.board, port=self.dev, pid=os.getpid(), link=self.ser is not None, suspended=self.suspended,
                        info=self.info, clients=sum(1 for c in self.clients if c.active), socket=self.sock_path,
                        loopback=self.loopback,
                        tty=[dict(path=t.path, pts=t.slave_name, policy=t.policy, open=t.is_open, uart=t.uart_on,
                                  settings=t.cur, note=t.note, **t.stats) for t in self.ttys],
                        swd_owner=[o.name if o else None for o in self.swd_owner])

    def _accept(self, srv):
        while not self.stop.is_set():
            try:
                s, _ = srv.accept()
            except OSError:
                if self.stop.is_set():
                    return
                continue
            self.nclients += 1
            c = Client(self, s, self.nclients)
            with self.lock:
                self.clients.add(c)

    # ---- TTY bridge -----------------------------------------------------------------------------------------
    def wake(self):
        try:
            os.write(self.ww, b"x")
        except BlockingIOError:
            pass

    def _tty_loop(self):
        last_check = 0.0
        while not self.stop.is_set():
            p = select.poll()
            p.register(self.wr, select.POLLIN)
            with self.lock:
                for t in self.ttys:
                    if t.is_open:
                        ev = (select.POLLIN if len(t.tx) < TX_HIGH else 0) | (select.POLLOUT if t.rx else 0)
                        if ev:
                            p.register(t.master, ev)
            for fd, ev in p.poll(20):
                if fd == self.wr:
                    try:
                        os.read(self.wr, 4096)
                    except BlockingIOError:
                        pass
                    continue
                t = next(t for t in self.ttys if t.master == fd)
                if ev & select.POLLHUP:
                    self._tty_check(t)   # the program closed it: take what it wrote, stop polling the master
                    continue
                with self.lock:
                    if ev & select.POLLIN:
                        try:
                            t.tx += os.read(fd, 65536)
                        except (BlockingIOError, OSError):
                            pass
                    if ev & select.POLLOUT and t.rx:
                        try:
                            n = os.write(fd, bytes(t.rx[:65536]))
                            del t.rx[:n]
                        except (BlockingIOError, OSError):
                            pass
            now = time.monotonic()
            if now - last_check >= 0.05:
                last_check = now
                for t in self.ttys:
                    self._tty_check(t)
            for t in self.ttys:
                self._tty_send(t)
            if self.ser is not None and now - self.last_info >= self.keepalive:
                self._probe()
            self._pump()   # requests held back by a pause go out once it ends

    def _tty_check(self, t):
        is_open = t.poll_open()
        with self.lock:
            if not is_open and len(t.tx) < TX_HIGH:
                # written before the program closed it (or between two of our looks): still goes out
                try:
                    t.tx += os.read(t.master, 65536)
                except OSError:
                    pass
            if t.policy == "off" and t.tx:
                t.stats["tx_dropped"] += len(t.tx)   # no bridge: input has nowhere to go
                t.tx.clear()
                t.txgen += 1
            if is_open != t.is_open:
                t.is_open = is_open
                if is_open:
                    t.stats["opens"] += 1
                    if t.backlog:
                        t.rx += t.backlog
                        t.backlog.clear()
                else:
                    t.rx.clear()   # nobody reads it any more
                    t.note = ""
            if t.busy or self.ser is None or time.monotonic() < t.retry_at:
                return
            want = t.want()
            if want:
                s = t.settings()
                if t.uart_on and s == t.cur:
                    return
                t.busy = True
                flags = P.UART_PARITY[s[1]] | (P.UART_STOP2 if s[2] else 0) | (P.UART_LOOPBACK if self.loopback
                                                                                else 0)
                payload = struct.pack("<BBII", P.UART_OPEN, t.ch, s[0], flags)
                cb = (lambda st, body, t=t, s=s: self._tty_opened(t, s, st, body))
            elif t.uart_on and not t.tx_inflight:
                t.busy = True
                payload = bytes([P.UART_CLOSE, t.ch])
                cb = (lambda st, body, t=t: self._tty_closed(t, st))
            else:
                return
        self.submit(payload, cb=cb)

    def _tty_opened(self, t, s, st, body):
        with self.lock:
            t.busy = False
            t.uart_on = st == 0
            t.cur = s if st == 0 else None
            t.opened_at = time.monotonic()
            t.note = "" if st == 0 else "UART_OPEN failed: %s" % P.ERRORS.get(st, st)
        if st == 0:
            actual = struct.unpack_from("<I", body)[0] if len(body) >= 4 else s[0]
            self.log("TTY %s: UART %d baud %s%s (actual %d)" % ("AB"[t.ch], s[0], s[1], " 2 stop" if s[2] else "",
                                                               actual))
        else:
            self.log("TTY %s: %s" % ("AB"[t.ch], t.note))
            t.retry_at = time.monotonic() + 1.0
        self.wake()

    def _tty_closed(self, t, st):
        with self.lock:
            t.busy = False
            t.uart_on = False
            t.cur = None
        self.log("TTY %s: UART closed" % "AB"[t.ch])

    def _tty_send(self, t):
        with self.lock:
            if not t.tx or t.tx_inflight or not t.uart_on or t.busy or time.monotonic() < t.tx_at:
                return
            chunk = bytes(t.tx[:UART_CHUNK])
            t.tx_inflight = True
            gen = t.txgen
        self.submit(bytes([P.UART_WRITE, t.ch]) + chunk, cb=lambda st, body, t=t, n=len(chunk), g=gen: self._tty_wrote(
            t, n, st, body, g))

    def _tty_wrote(self, t, n, st, body, gen):
        with self.lock:
            t.tx_inflight = False
            if gen != t.txgen:
                pass   # tx was dropped meanwhile: this ack is about bytes that are gone
            elif st == 0:
                k = struct.unpack_from("<H", body)[0] if len(body) >= 2 else 0
                del t.tx[:k]
                t.stats["tx"] += k
                t.note = ""
                if k < n and t.cur:
                    # the board's 8 KB TX buffer is full: come back once a quarter of it went out on the wire,
                    # instead of sending the same bytes over USB again and again
                    t.tx_at = time.monotonic() + 2048 / (t.cur[0] / 10.0)
            elif st == P.ERR_INTERLOCK and time.monotonic() - t.opened_at < INTERLOCK_GRACE:
                t.tx_at = time.monotonic() + 0.02   # just opened: the switches close after VTref settled
            elif st == P.ERR_INTERLOCK:
                t.stats["tx_dropped"] += len(t.tx)   # no DUT power or switch mode off: like an unplugged cable
                t.tx.clear()
                t.txgen += 1
                t.note = "switches open (no VTref, or mode off): TX dropped"
            elif st == P.ERR_STATE:
                # the board closed the stream (host loss, restart): open it again rather than write into nothing
                if t.uart_on:
                    self.log("TTY %s: the board closed the UART: reopening" % "AB"[t.ch])
                t.uart_on, t.cur = False, None
                t.retry_at = time.monotonic() + 0.2
        self.wake()

    # ---- run ------------------------------------------------------------------------------------------------
    def serve(self):
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.sock_path)
        os.chmod(self.sock_path, 0o600)
        srv.listen(16)
        self.log("socket %s" % self.sock_path)
        for t in self.ttys:
            self.log("TTY %s: %s -> %s (policy %s, %s%s)" % ("AB"[t.ch], t.path, t.slave_name, t.policy,
                                                           "8%s%d" % (t.parity, 2 if t.stop2 else 1),
                                                           ", baud from the TTY"))
        ths = [threading.Thread(target=self._reader, name="board-rx", daemon=True),
               threading.Thread(target=self._accept, args=(srv,), name="accept", daemon=True),
               threading.Thread(target=self._tty_loop, name="tty", daemon=True)]
        for th in ths:
            th.start()
        try:
            while not self.stop.is_set():
                self.stop.wait(0.5)
        finally:
            self.shutdown(srv)

    def shutdown(self, srv=None):
        self.stop.set()
        if srv:
            srv.close()
        with self.lock:
            ser = self.ser
        if ser is not None:   # close the UARTs cleanly; the board isolates the rest on host loss
            try:
                for ch in (0, 1):
                    ser.write(P.frame_encode(bytes([0, P.UART_CLOSE, ch])))
                time.sleep(0.3)
                ser.close()
            except (serial.SerialException, OSError):
                pass
        for t in self.ttys:
            t.close()
        for p in (self.sock_path,):
            try:
                os.unlink(p)
            except OSError:
                pass


def run(port=None, policies=("open", "open"), fmts=(("N", False), ("N", False)), log=None, loopback=False):
    """Run the daemon in the foreground until SIGINT/SIGTERM."""
    log = log or (lambda s: print(time.strftime("%H:%M:%S"), s, flush=True))
    d = Daemon(port, policies, fmts, log, loopback=loopback)
    if loopback:
        log("TEST MODE: UARTs in internal loopback, the DUT pads are not used")
    signal.signal(signal.SIGTERM, lambda *a: d.stop.set())
    signal.signal(signal.SIGINT, lambda *a: d.stop.set())
    d.serve()
    return 0


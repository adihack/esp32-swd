# SPDX-License-Identifier: MIT
"""esp-swd host client over a serial transport (r5 test firmware: USB-Serial-JTAG CDC).

A reader thread owns the receive side: it matches responses to requests by sequence number and collects UART RX
events per channel, so requests may come from several threads and UART data arrives while a request is running.
While idle, the client sends a PING every `keepalive` seconds: the firmware treats a silent host as gone and
isolates its channels (a listen-only UART sniffer would otherwise be cut off after a few seconds).
"""
import struct
import threading
import time
import zlib

import serial

from . import proto as P


class EswError(Exception):
    def __init__(self, cmd, status, data=b""):
        self.cmd, self.status, self.data = cmd, status, data
        super().__init__("cmd 0x%02X failed: %s (%d)" % (cmd, P.ERRORS.get(status, "?"), status))


class Esw:
    MAX_WORDS = 512
    UART_CHUNK = 2048       # data bytes per UART_WRITE frame
    UART_BUF_MAX = 64 << 20   # per channel, if nobody reads: oldest data is dropped beyond this

    def __init__(self, port="/dev/ttyACM0", timeout=10.0, keepalive=1.0):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.timeout = timeout
        self.keepalive = keepalive
        self.seq = 0
        self.bad_frames = 0
        self.ser.reset_input_buffer()
        self._tx_lock = threading.Lock()
        self._cv = threading.Condition()
        self._pending = {}      # seq -> cmd, for requests waiting for a response
        self._done = {}         # seq -> (status, body)
        self._inflight = {}     # seq -> (budget key, request bytes the board has not answered yet)
        self._inflight_bytes = {}   # budget key -> bytes: "dev", or a channel number on firmware with proto >= 4
        self.proto = 0          # the board's protocol version, read below
        self._error = None
        self._last_tx = time.monotonic()
        self._ucv = threading.Condition()
        self._uart = [bytearray(), bytearray()]
        self.uart_events = [0, 0]   # RX event frames received
        self.uart_flags = [0, 0]    # OR of every ESW_UART_RX_* flag seen
        self.uart_trimmed = [0, 0]  # bytes dropped here because nobody read them
        self.uart_backend = ["off", "off"]
        self.regions = {}        # per channel: memory regions of the last attach
        self.on_uart = None         # optional callback(ch, ts_us, flags, data) on the reader thread; replaces buffering
        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, name="esw-reader", daemon=True)
        self._reader.start()
        try:
            self.proto = self.info()["proto"]
        except Exception:  # noqa: BLE001 - an old or busy board: keep the conservative single budget
            self.proto = 0

    def close(self):
        self._stop.set()
        self._reader.join(timeout=2)
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- transport ---------------------------------------------------------------------------------------
    window = 1   # frames in flight for bulk commands (program/verify/mem_write); responses come back in order
    MAX_WINDOW = 3   # the board's USB RX ring is 8 KB with no flow control: at most 3 full frames fit
    # The same limit across threads: unanswered request bytes stay below this. The reader thread returns a frame's
    # bytes as soon as its response arrives, so no thread can hold credit hostage. Firmware with proto >= 4 moves
    # channel requests straight into a per-channel queue of CH_QUEUE frames: each channel then has its own budget
    # and a slow channel no longer starves the other.
    RX_BUDGET = 7168
    CH_BUDGET = P.CH_QUEUE * 2310   # CH_QUEUE full frames (2 KB data + header + COBS + CRC + delimiter)

    def _budget(self, cmd, data):
        if self.proto >= 4 and data and data[0] < 2 and (cmd in (P.CH_HOLD, P.CH_RELEASE) or 0x20 <= cmd <= 0x2F):
            return data[0], self.CH_BUDGET
        return "dev", self.RX_BUDGET

    def _read_loop(self):
        buf = bytearray()
        try:
            while not self._stop.is_set():
                chunk = self.ser.read(self.ser.in_waiting or 1)   # returns as soon as bytes arrive
                if chunk:
                    buf += chunk
                    while True:
                        i = buf.find(0)
                        if i < 0:
                            break
                        raw = bytes(buf[:i])
                        del buf[:i + 1]
                        if raw:
                            self._dispatch(raw)
                    if len(buf) > 65536:   # no delimiter in 64 KB: garbage, resynchronise
                        buf.clear()
                        self.bad_frames += 1
                if self.keepalive and time.monotonic() - self._last_tx > self.keepalive:
                    self._send(P.PING, b"", track=False)
        except Exception as e:  # noqa: BLE001 - serial gone (unplug, close): wake every waiter with the error
            if not self._stop.is_set():
                self._error = e
        finally:
            with self._cv:
                self._cv.notify_all()
            with self._ucv:
                self._ucv.notify_all()

    def _dispatch(self, raw):
        pl = P.frame_decode(raw)
        if pl is None or len(pl) < 3:
            self.bad_frames += 1   # e.g. boot ROM text after a reset
            return
        cmd = pl[1]
        if P.EVT_FIRST <= cmd <= P.EVT_LAST:
            if cmd == P.EVT_UART_RX and len(pl) >= 9 and pl[3] < 2:
                self._uart_event(pl[3], pl[4], struct.unpack_from("<I", pl, 5)[0], pl[9:])
            return
        with self._cv:
            if self._pending.get(pl[0]) == (cmd & 0x7F) and cmd & 0x80:
                self._done[pl[0]] = (struct.unpack("b", pl[2:3])[0], pl[3:])
                self._release(pl[0])   # the board consumed the request
                self._cv.notify_all()

    def _uart_event(self, ch, flags, ts, data):
        self.uart_events[ch] += 1
        self.uart_flags[ch] |= flags
        cb = self.on_uart
        if cb:
            cb(ch, ts, flags, data)
            return
        with self._ucv:
            b = self._uart[ch]
            b += data
            if len(b) > self.UART_BUF_MAX:
                cut = len(b) - self.UART_BUF_MAX
                del b[:cut]
                self.uart_trimmed[ch] += cut
            self._ucv.notify_all()

    def _send(self, cmd, data=b"", track=True):
        n = len(data) + len(data) // 254 + 8   # encoded frame size, upper bound
        key, limit = self._budget(cmd, data)
        if track:
            with self._cv:
                deadline = time.monotonic() + self.timeout
                while self._inflight_bytes.get(key, 0) and self._inflight_bytes[key] + n > limit:
                    if self._error:
                        raise ConnectionError("esp-swd link lost: %s" % self._error)
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise TimeoutError("no responses: %d request bytes unanswered" % self._inflight_bytes[key])
                    self._cv.wait(left)
                self._inflight_bytes[key] = self._inflight_bytes.get(key, 0) + n
        with self._tx_lock:
            self.seq = (self.seq + 1) & 0xFF
            seq = self.seq
            if track:
                with self._cv:
                    self._pending[seq] = cmd
                    self._done.pop(seq, None)
                    self._inflight[seq] = (key, n)
            self.ser.write(P.frame_encode(bytes([seq, cmd]) + data))
            self._last_tx = time.monotonic()
        return seq

    def _release(self, seq):   # with self._cv held
        key, n = self._inflight.pop(seq, (None, 0))
        if key is not None:
            self._inflight_bytes[key] -= n

    def _recv(self, seq, cmd, timeout):
        deadline = time.monotonic() + timeout
        with self._cv:
            try:
                while seq not in self._done:
                    if self._error:
                        raise ConnectionError("esp-swd link lost: %s" % self._error)
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise TimeoutError("no response to cmd 0x%02X" % cmd)
                    self._cv.wait(left)
                return self._done.pop(seq)
            finally:
                self._pending.pop(seq, None)
                self._release(seq)   # timed out: give the credit back
                self._cv.notify_all()

    def request(self, cmd, data=b"", timeout=None, check=True):
        seq = self._send(cmd, data)
        status, body = self._recv(seq, cmd, timeout or self.timeout)
        if check and status != 0:
            raise EswError(cmd, status, body)
        return body if check else (status, body)

    def _pipelined(self, cmd, payloads, timeout):
        """Send payloads with up to self.window frames in flight; raise on the first error status."""
        inflight = []
        for pl in payloads:
            inflight.append(self._send(cmd, pl))
            if len(inflight) >= max(1, min(self.window, P.CH_QUEUE if self.proto >= 4 else self.MAX_WINDOW)):
                st, body = self._recv(inflight.pop(0), cmd, timeout)
                if st != 0:
                    raise EswError(cmd, st, body)
        while inflight:
            st, body = self._recv(inflight.pop(0), cmd, timeout)
            if st != 0:
                raise EswError(cmd, st, body)

    # ---- device ----------------------------------------------------------------------------------------------
    def ping(self, data=b"esw"):
        return self.request(P.PING, data)

    def info(self):
        d = self.request(P.INFO)
        proto, up, rr, heap = struct.unpack_from("<IIII", d)
        return dict(proto=proto, uptime_ms=up, reset_reason=rr, free_heap=heap,
                    fw=d[16:48].split(b"\x00")[0].decode())

    def reboot(self, rom_download=False):
        self.request(P.REBOOT, bytes([1 if rom_download else 0]))

    # ---- channel / isolation -------------------------------------------------------------------------------------
    def hold(self, ch, group):
        self.request(P.CH_HOLD, bytes([ch, group]))

    def release(self, ch, group):
        self.request(P.CH_RELEASE, bytes([ch, group]))

    def state(self, ch):
        d = self.request(P.CH_STATE, bytes([ch]))
        mv, vt_closed, swd, uart, vt_valid, h_swd, h_uart = struct.unpack_from("<iBBBBII", d)
        return dict(vtref_mv=mv if vt_valid else None, vtref_closed=bool(vt_closed), swd=P.GSTATE[swd],
                    uart=P.GSTATE[uart], holders_swd=h_swd, holders_uart=h_uart)

    def wait_group(self, ch, group, want="CLOSED", timeout=2.0):
        key = {P.GROUP_SWD: "swd", P.GROUP_UART: "uart"}[group]
        t0 = time.monotonic()
        while True:
            st = self.state(ch)
            if st[key] == want or time.monotonic() - t0 > timeout:
                return st
            time.sleep(0.02)

    # ---- SWD / target ------------------------------------------------------------------------------------------
    def attach(self, ch, clock=0, phy=1, core=0, raw=False):
        """phy 1 (default): ESP32-S3 dedicated GPIO, clock in Hz (0 = 8 MHz); phy 0: portable bit-bang (clock =
        half-period busy loops). The firmware reports the actual dgpio clock in khz. core: multi-core parts
        (nRF5340: 0 application, 1 network); the generic Cortex-M module takes it as the AP index. raw: skip the
        chip modules, attach the generic module to AP <core> (diagnosis and recovery)."""
        st, d = self.request(P.SWD_ATTACH, struct.pack("<BIBB", ch, clock, phy, core | (0x80 if raw else 0)),
                             check=False)
        return self._target_info(P.SWD_ATTACH, ch, st, d)

    def _target_info(self, cmd, ch, st, d):
        if len(d) < 40:   # argument errors come back without a body
            raise EswError(cmd, st, d)
        dpidr, locked, nreg, khz = struct.unpack_from("<IBBH", d)
        part = d[8:40].split(b"\x00")[0].decode()
        regions = []
        for i in range(nreg):
            start, size, sector, kind = struct.unpack_from("<IIII", d, 40 + 16 * i)
            regions.append(dict(start=start, size=size, sector=sector, kind=P.MEM_KIND.get(kind, kind)))
        info = dict(status=st, dpidr=dpidr, locked=bool(locked), part=part, regions=regions, khz=khz)
        if st != 0:
            raise EswError(cmd, st, d)
        self.regions[ch] = regions
        return info

    def detach(self, ch):
        self.request(P.SWD_DETACH, bytes([ch]))

    def unlock(self, ch, allow_erase=False):
        """Regain access to a protected part (allow_erase: by mass erase). Returns the attach info as it is now; do
        not attach again before programming: that resets the part, and an erased nRF5340 shuts its port again."""
        st, d = self.request(P.TGT_UNLOCK, struct.pack("<BI", ch, P.UNLOCK_ALLOW_ERASE if allow_erase else 0),
                             timeout=60, check=False)
        return self._target_info(P.TGT_UNLOCK, ch, st, d)

    def reset(self, ch, halt=False):
        self.request(P.TGT_RESET, struct.pack("<BI", ch, P.RESET_HALT if halt else 0))

    def mem_read(self, ch, addr, nwords):
        out = []
        while nwords:
            n = min(nwords, self.MAX_WORDS)
            d = self.request(P.MEM_READ, struct.pack("<BIH", ch, addr, n))
            out += struct.unpack("<%dI" % n, d[:4 * n])
            addr, nwords = addr + 4 * n, nwords - n
        return out

    def _words_cmd(self, cmd, ch, addr, data: bytes, timeout=30):
        if len(data) % 4:
            data += b"\xff" * (4 - len(data) % 4)
        step = 4 * self.MAX_WORDS
        self._pipelined(cmd, (struct.pack("<BI", ch, addr + off) + data[off:off + step]
                              for off in range(0, len(data), step)), timeout)

    def mem_write(self, ch, addr, data: bytes):
        self._words_cmd(P.MEM_WRITE, ch, addr, data)

    def erase(self, ch, addr, length):
        self.request(P.TGT_ERASE, struct.pack("<BII", ch, addr, length), timeout=60)

    def program(self, ch, addr, data: bytes):
        self._words_cmd(P.TGT_PROGRAM, ch, addr, data)

    def verify(self, ch, addr, data: bytes):
        self._words_cmd(P.TGT_VERIFY, ch, addr, data)

    def bench(self, ch, mode, n):
        """Device-side PHY benchmark. mode 0: n DPIDR reads, 1: n-word RAM block write, 2: n-word read + check.
        The status is returned, not raised, so a failing clock still reports its error counters."""
        st, d = self.request(P.SWD_BENCH, struct.pack("<BBI", ch, mode, n), timeout=60, check=False)
        x = struct.unpack_from("<7I", d) if len(d) >= 28 else (0,) * 7
        return dict(status=st, us=x[0], errors=x[1], packets=x[2], wait=x[3], fault=x[4], noack=x[5], parity=x[6])

    def test_flash_stress(self, ms=0, gap_ms=250):
        """Test firmware only: erase+write a flash sector every gap_ms in the background for ms (0: report).
        -> (ops, busy)"""
        d = self.request(0x7D, struct.pack("<II", int(ms), int(gap_ms)))
        return struct.unpack_from("<IB", d)

    def test_panic(self):
        """Test firmware only: deliberate abort() after replying (checks that a crash isolates and recovers)."""
        self.request(0x7E)

    def crc32(self, ch, addr, length, reset_halt=True):
        """zlib.crc32 of target memory, computed on the target by the built-in CRC loader where possible."""
        d = self.request(P.TGT_CRC32, struct.pack("<BIII", ch, addr, length, P.CRC_RESET_HALT if reset_halt else 0),
                         timeout=120)
        return struct.unpack_from("<I", d)[0]

    def dap_read(self, ch, reg, ap=None):
        """Raw DP (ap=None; reg bits 7:4 = DPBANKSEL) or AP register read on an attached session."""
        d = self.request(P.DAP_RAW, struct.pack("<BBBBI", ch, 0 if ap is None else 2, ap or 0, reg, 0))
        return struct.unpack_from("<I", d)[0]

    def dap_write(self, ch, reg, value, ap=None):
        self.request(P.DAP_RAW, struct.pack("<BBBBI", ch, 1 if ap is None else 3, ap or 0, reg, value))

    def stats(self, ch):
        x = struct.unpack("<6I", self.request(P.SWD_STATS, bytes([ch])))
        return dict(xfers=x[0], wait=x[1], fault=x[2], noack=x[3], parity=x[4], last_op_us=x[5])

    @staticmethod
    def data_segments(image: bytes, gap=256, unit=16):
        """[(start, end)] byte ranges of image holding data: 0xFF stretches of at least `gap` bytes (in `unit`
        steps) are left out. Shorter blank stretches stay in, so the number of segments stays small."""
        n, blank = len(image), b"\xff" * unit
        segs, cur, blank_run = [], None, 0
        for b in range((n + unit - 1) // unit):
            chunk = image[b * unit:(b + 1) * unit]
            if chunk == blank[:len(chunk)]:
                blank_run += 1
                continue
            if cur is None:
                cur = [b, b + 1]
            elif blank_run * unit >= gap:
                segs.append(cur)
                cur = [b, b + 1]
            else:
                cur[1] = b + 1
            blank_run = 0
        if cur:
            segs.append(cur)
        return [(a * unit, min(e * unit, n)) for a, e in segs]

    def program_segments(self, ch, addr, image: bytes, segs):
        """Program only the given (start, end) byte ranges of image, as one pipelined stream."""
        step = 4 * self.MAX_WORDS
        self._pipelined(P.TGT_PROGRAM, (struct.pack("<BI", ch, addr + off) + image[off:min(off + step, e)]
                                        for a, e in segs for off in range(a, e, step)), 30)

    def flash(self, ch, addr, image: bytes, page=None, verify="crc", skip_blank=256):
        """Erase the pages the image touches, program it, verify it (on-target CRC, or read-back). page: erase
        granularity; default: the sector size of the region reported at attach. skip_blank: 0xFF stretches of at
        least this many bytes are not sent (the erase left them 0xFF; the CRC still covers them); 0 sends all."""
        if page is None:
            page = next((r["sector"] for r in self.regions.get(ch, ())
                         if r["start"] <= addr < r["start"] + r["size"]), 4096)
        if len(image) % 4:
            image += b"\xff" * (4 - len(image) % 4)
        start, end = addr - addr % page, addr + len(image)
        end += (-end) % page
        self.erase(ch, start, end - start)
        if skip_blank and addr % 16 == 0:   # segments stay quad-word aligned (STM32U5 programs 16 bytes at once)
            segs = self.data_segments(image, skip_blank)
            for a, e in segs:   # padded to whole words
                assert a % 4 == 0
            self.program_segments(ch, addr, image, [(a, e + (-e) % 4) for a, e in segs])
        else:
            self.program(ch, addr, image)
        if verify == "crc":
            got, want = self.crc32(ch, addr, len(image)), zlib.crc32(image)
            if got != want:
                raise EswError(P.TGT_CRC32, -9, struct.pack("<II", got, want))
        else:
            self.verify(ch, addr, image)

    # ---- DUT UART ------------------------------------------------------------------------------------------
    def uart_open(self, ch, baud=115200, parity="N", stopbits=1, rx_filter=True, tx_strong=False, dma=True):
        """Configure the DUT UART and start streaming RX. The firmware holds the UART group: its switches close as
        soon as VTref is valid (wait_group(ch, GROUP_UART) to know). Calling it again changes the settings.
        rx_filter: ignore RX pulses shorter than 1/8 bit (crosstalk from our own TX edges); tx_strong: full TX drive
        strength instead of the weakest; dma: may take the single DMA engine if it is free (open the faster channel
        first). Returns the actual baud rate; uart_backend[ch] tells "dma" or "irq"."""
        flags = (P.UART_PARITY[parity] | (P.UART_STOP2 if stopbits == 2 else 0) |
                 (0 if rx_filter else P.UART_RX_NOFILT) | (P.UART_TX_STRONG if tx_strong else 0) |
                 (0 if dma else P.UART_NO_DMA))
        d = self.request(P.UART_OPEN, struct.pack("<BII", ch, int(baud), flags))
        self.uart_backend[ch] = P.UART_BACKEND.get(d[4], "?") if len(d) > 4 else "irq"
        return struct.unpack_from("<I", d)[0]

    def uart_close(self, ch):
        self.request(P.UART_CLOSE, bytes([ch]))

    def uart_write(self, ch, data: bytes, timeout=30.0):
        """Send all of data; waits while the board's TX buffer is full."""
        view, deadline = memoryview(bytes(data)), time.monotonic() + timeout
        while len(view):
            chunk = view[:self.UART_CHUNK]
            n = struct.unpack("<H", self.request(P.UART_WRITE, bytes([ch]) + bytes(chunk))[:2])[0]
            view = view[n:]
            if n < len(chunk):
                if time.monotonic() > deadline:
                    raise TimeoutError("UART %d: TX stalled" % ch)
                time.sleep(0.002)

    def uart_read(self, ch, n=None, timeout=1.0):
        """Up to n received bytes (n None: whatever is there once the first byte arrived). Returns b"" on timeout."""
        deadline = time.monotonic() + timeout
        with self._ucv:
            b = self._uart[ch]
            while (len(b) < n) if n is not None else not b:
                left = deadline - time.monotonic()
                if left <= 0 or self._error:
                    break
                self._ucv.wait(left)
            k = len(b) if n is None else min(n, len(b))
            out = bytes(b[:k])
            del b[:k]
            return out

    def uart_read_until(self, ch, token=b"\n", timeout=2.0):
        """Received bytes up to and including token; TimeoutError (with the tail seen) if it does not arrive."""
        deadline = time.monotonic() + timeout
        with self._ucv:
            b = self._uart[ch]
            while True:
                i = b.find(token)
                if i >= 0:
                    out = bytes(b[:i + len(token)])
                    del b[:i + len(token)]
                    return out
                left = deadline - time.monotonic()
                if left <= 0 or self._error:
                    raise TimeoutError("UART %d: %r not seen; buffer tail %r" % (ch, token, bytes(b[-80:])))
                self._ucv.wait(left)

    def uart_pending(self, ch):
        with self._ucv:
            return len(self._uart[ch])

    def uart_clear(self, ch):
        with self._ucv:
            self._uart[ch].clear()

    def uart_stats(self, ch):
        d = self.request(P.UART_STATS, bytes([ch]))
        x = struct.unpack_from("<BBBB10I", d)
        return dict(open=bool(x[0]), routed=bool(x[1]), backend=P.UART_BACKEND.get(x[2], "?"), baud=x[4], rx=x[5],
                    tx=x[6], usb_drop=x[7], fifo_ovf=x[8], buf_full=x[9], frame_err=x[10], parity_err=x[11],
                    brk=x[12], dma_err=x[13])

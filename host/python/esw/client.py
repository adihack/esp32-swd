# SPDX-License-Identifier: MIT
"""esp-swd host client over a serial transport (r5 test firmware: USB-Serial-JTAG CDC)."""
import struct
import time

import serial

from . import proto as P


class EswError(Exception):
    def __init__(self, cmd, status, data=b""):
        self.cmd, self.status, self.data = cmd, status, data
        super().__init__("cmd 0x%02X failed: %s (%d)" % (cmd, P.ERRORS.get(status, "?"), status))


class Esw:
    MAX_WORDS = 512

    def __init__(self, port="/dev/ttyACM0", timeout=10.0):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.timeout = timeout
        self.seq = 0
        self.rx = bytearray()
        self.bad_frames = 0
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    # ---- transport ---------------------------------------------------------------------------------------
    window = 1   # frames in flight for bulk commands (program/verify/mem_write); responses come back in order
    MAX_WINDOW = 3   # the board's USB RX ring is 8 KB with no flow control: at most 3 full frames fit

    def _send(self, cmd, data=b""):
        self.seq = (self.seq + 1) & 0xFF
        self.ser.write(P.frame_encode(bytes([self.seq, cmd]) + data))
        return self.seq

    def _recv(self, seq, cmd, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1)   # return as soon as bytes arrive
            if chunk:
                self.rx += chunk
            while b"\x00" in self.rx:
                raw, _, rest = bytes(self.rx).partition(b"\x00")
                self.rx = bytearray(rest)
                if not raw:
                    continue
                pl = P.frame_decode(raw)
                if pl is None:
                    self.bad_frames += 1   # e.g. boot ROM text after a reset
                    continue
                if len(pl) >= 3 and pl[0] == seq and pl[1] == (cmd | 0x80):
                    return struct.unpack("b", pl[2:3])[0], pl[3:]
        raise TimeoutError("no response to cmd 0x%02X" % cmd)

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
            if len(inflight) >= max(1, min(self.window, self.MAX_WINDOW)):
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
    def attach(self, ch, clock=0, phy=1):
        """phy 1 (default): ESP32-S3 dedicated GPIO, clock in Hz (0 = 8 MHz); phy 0: portable bit-bang (clock =
        half-period busy loops). The firmware reports the actual dgpio clock in khz."""
        st, d = self.request(P.SWD_ATTACH, struct.pack("<BIB", ch, clock, phy), check=False)
        if len(d) < 40:   # argument errors come back without a body
            raise EswError(P.SWD_ATTACH, st, d)
        dpidr, locked, nreg, khz = struct.unpack_from("<IBBH", d)
        part = d[8:40].split(b"\x00")[0].decode()
        regions = []
        for i in range(nreg):
            start, size, sector, kind = struct.unpack_from("<IIII", d, 40 + 16 * i)
            regions.append(dict(start=start, size=size, sector=sector, kind=P.MEM_KIND.get(kind, kind)))
        info = dict(status=st, dpidr=dpidr, locked=bool(locked), part=part, regions=regions, khz=khz)
        if st != 0:
            raise EswError(P.SWD_ATTACH, st, d)
        return info

    def detach(self, ch):
        self.request(P.SWD_DETACH, bytes([ch]))

    def unlock(self, ch, allow_erase=False):
        self.request(P.TGT_UNLOCK, struct.pack("<BI", ch, P.UNLOCK_ALLOW_ERASE if allow_erase else 0), timeout=20)

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

    def test_panic(self):
        """Test firmware only: deliberate abort() after replying (checks that a crash isolates and recovers)."""
        self.request(0x7E)

    def crc32(self, ch, addr, length, reset_halt=True):
        """zlib.crc32 of target memory, computed on the target by the built-in CRC loader where possible."""
        d = self.request(P.TGT_CRC32, struct.pack("<BIII", ch, addr, length, P.CRC_RESET_HALT if reset_halt else 0),
                         timeout=120)
        return struct.unpack_from("<I", d)[0]

    def stats(self, ch):
        x = struct.unpack("<6I", self.request(P.SWD_STATS, bytes([ch])))
        return dict(xfers=x[0], wait=x[1], fault=x[2], noack=x[3], parity=x[4], last_op_us=x[5])

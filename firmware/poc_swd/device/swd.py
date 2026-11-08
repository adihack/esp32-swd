# SPDX-License-Identifier: MIT
# swd.py - minimal ADIv5 SWD host, bit-banged with machine.Pin (PoC: ~30 kHz, no dedicated GPIO yet).
# Bit timing follows CMSIS-DAP SW_DP.c: host changes SWDIO while SWCLK is low, target samples on the rising
# edge; host samples SWDIO after the falling edge (data the target launched on the previous rising edge).
from machine import Pin
import time

ACK_OK = 1
ACK_WAIT = 2
ACK_FAULT = 4

DP_DPIDR = 0x0      # R
DP_ABORT = 0x0      # W
DP_CTRL_STAT = 0x4  # RW (SELECT.DPBANKSEL = 0)
DP_SELECT = 0x8     # W
DP_RDBUFF = 0xC     # R

AP_CSW = 0x00
AP_TAR = 0x04
AP_DRW = 0x0C
AP_IDR = 0xFC


def ack_name(ack):
    return {1: 'OK', 2: 'WAIT', 4: 'FAULT',
            7: 'none (SWDIO stayed high)', 0: 'none (SWDIO stayed low)'}.get(ack, 'invalid 0b{:03b}'.format(ack))


def parity(v):
    return bin(v).count('1') & 1


class SWDError(Exception):
    def __init__(self, msg, ack=None):
        super().__init__(msg)
        self.ack = ack


class SWD:
    def __init__(self, swclk, swdio, half_period_us=0, drive=Pin.DRIVE_0, wait_retries=50):
        self.drive = drive
        self.clk = Pin(swclk, Pin.OUT, value=0, pull=None, drive=drive)
        self.dio = Pin(swdio, Pin.IN, pull=None)
        self.dio_is_out = False
        self.hp = half_period_us
        self.wait_retries = wait_retries
        self.select = None
        self.stats = {'xfers': 0, 'wait': 0, 'fault': 0, 'noack': 0, 'parity': 0}

    # ---- bit level ------------------------------------------------------------------------------
    def _out(self, level):
        if self.dio_is_out:
            self.dio(level)
        else:
            self.dio.init(Pin.OUT, value=level, pull=None, drive=self.drive)
            self.dio_is_out = True

    def _in(self):
        if self.dio_is_out:
            self.dio.init(Pin.IN, pull=None)
            self.dio_is_out = False

    def _cycles(self, n):
        clk = self.clk
        hp = self.hp
        for _ in range(n):
            clk(0)
            if hp:
                time.sleep_us(hp)
            clk(1)
            if hp:
                time.sleep_us(hp)

    def _write(self, value, n):
        clk = self.clk
        dio = self.dio
        hp = self.hp
        for _ in range(n):
            dio(value & 1)
            clk(0)
            if hp:
                time.sleep_us(hp)
            clk(1)
            if hp:
                time.sleep_us(hp)
            value >>= 1

    def _read(self, n):
        clk = self.clk
        dio = self.dio
        hp = self.hp
        v = 0
        for i in range(n):
            clk(0)
            if hp:
                time.sleep_us(hp)
            if dio():
                v |= 1 << i
            clk(1)
            if hp:
                time.sleep_us(hp)
        return v

    # ---- sequences ------------------------------------------------------------------------------
    def line_reset(self, n=56):
        self._out(1)
        self._cycles(n)

    def idle(self, n=8):
        self._out(0)
        self._cycles(n)

    def jtag_to_swd(self):
        """Line reset, 0xE79E JTAG-to-SWD switch, line reset, idle. Leaves the DP in reset state."""
        self.line_reset()
        self._out(0)
        self._write(0xE79E, 16)
        self.line_reset()
        self.idle(8)
        self.select = None

    def end(self):
        """Idle cycles, then leave SWCLK low and SWDIO as input (tool-side safe state)."""
        self.idle(8)
        self.clk(0)
        self._in()

    # ---- packet level ---------------------------------------------------------------------------
    def raw(self, apndp, rnw, addr, wdata=0):
        """One SWD packet. Returns (ack, data). Never raises."""
        a2 = (addr >> 2) & 1
        a3 = (addr >> 3) & 1
        req = 0x81 | (apndp << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) | (((apndp + rnw + a2 + a3) & 1) << 5)
        self.stats['xfers'] += 1
        self._out(1)
        self._write(req, 8)
        self._in()
        self._cycles(1)                          # turnaround host -> target
        ack = self._read(3)
        if ack == ACK_OK:
            if rnw:
                data = self._read(32)
                p = self._read(1)
                self._cycles(1)                  # turnaround target -> host
                self._out(0)
                if p != parity(data):
                    self.stats['parity'] += 1
                    return (-1, data)            # -1: parity error
            else:
                self._cycles(1)
                self._out(0)
                self._write(wdata, 32)
                self._write(parity(wdata), 1)
                data = None
            self.idle(2)
            return (ack, data)
        if ack in (ACK_WAIT, ACK_FAULT):
            self._cycles(1)
            self._out(0)
            return (ack, None)
        # protocol error / no target: back off a full data phase, then take the line back
        self._cycles(1 + 32 + 1)
        self._out(1)
        return (ack, None)

    def xfer(self, apndp, rnw, addr, wdata=0):
        for _ in range(self.wait_retries):
            ack, data = self.raw(apndp, rnw, addr, wdata)
            if ack == ACK_OK:
                return data
            if ack == ACK_WAIT:
                self.stats['wait'] += 1
                continue
            what = '%s %s 0x%X' % ('AP' if apndp else 'DP', 'read' if rnw else 'write', addr)
            if ack == -1:
                raise SWDError('%s: parity error (data 0x%08X)' % (what, data), ack)
            if ack == ACK_FAULT:
                self.stats['fault'] += 1
            else:
                self.stats['noack'] += 1
            raise SWDError('%s: ACK %s' % (what, ack_name(ack)), ack)
        raise SWDError('WAIT retries exhausted', ACK_WAIT)

    # ---- DP / AP ----------------------------------------------------------------------------------
    def dp_read(self, addr):
        return self.xfer(0, 1, addr)

    def dp_write(self, addr, val):
        self.xfer(0, 0, addr, val)
        if addr == DP_SELECT:
            self.select = val

    def clear_errors(self):
        self.dp_write(DP_ABORT, 0x1E)          # STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR

    def connect(self):
        """Switch to SWD and read DPIDR. Raises SWDError on no/invalid response."""
        self.jtag_to_swd()
        return self.dp_read(DP_DPIDR)

    def power_up(self, timeout_ms=200):
        self.clear_errors()
        self.dp_write(DP_SELECT, 0)
        self.dp_write(DP_CTRL_STAT, 0x50000000)  # CSYSPWRUPREQ | CDBGPWRUPREQ
        t0 = time.ticks_ms()
        while True:
            cs = self.dp_read(DP_CTRL_STAT)
            if (cs & 0xA0000000) == 0xA0000000:  # CSYSPWRUPACK | CDBGPWRUPACK
                return cs
            if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
                raise SWDError('power-up ACK timeout, CTRL/STAT=0x%08X' % cs)

    def power_down(self):
        self.dp_write(DP_SELECT, 0)
        self.dp_write(DP_CTRL_STAT, 0)

    def _ap_sel(self, ap, reg):
        sel = (ap << 24) | (reg & 0xF0)
        if sel != self.select:
            self.dp_write(DP_SELECT, sel)

    def ap_read(self, ap, reg):
        self._ap_sel(ap, reg)
        self.xfer(1, 1, reg & 0x0C)              # posted: result arrives with the next read
        return self.dp_read(DP_RDBUFF)

    def ap_write(self, ap, reg, val):
        self._ap_sel(ap, reg)
        self.xfer(1, 0, reg & 0x0C, val)

    # ---- MEM-AP (32-bit, no auto-increment) ------------------------------------------------------
    def mem_init(self, ap=0, csw=0x23000002):
        self.ap_write(ap, AP_CSW, csw)
        return self.ap_read(ap, AP_CSW)

    def read32(self, addr, ap=0):
        self.ap_write(ap, AP_TAR, addr)
        return self.ap_read(ap, AP_DRW)

    def write32(self, addr, val, ap=0):
        self.ap_write(ap, AP_TAR, addr)
        self.ap_write(ap, AP_DRW, val)
        self.dp_read(DP_RDBUFF)                  # make sure the write completed

    def measure_hz(self, n=5):
        t0 = time.ticks_us()
        for _ in range(n):
            self.dp_read(DP_DPIDR)
        dt = time.ticks_diff(time.ticks_us(), t0)
        return int(n * 46 / (dt / 1e6)) if dt else 0   # ~46 clocks per read packet incl. idle

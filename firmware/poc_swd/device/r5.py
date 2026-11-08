# SPDX-License-Identifier: MIT
# r5.py - board r5 pin map, safe state and per-channel switch control.
# Source of truth: docs/hardware-reference.md. Rules kept here (CLAUDE.md):
#  * EN pins are configured as outputs driven LOW before anything else, never with internal pull-ups.
#  * EN_SWD / EN_UART close only after EN_VTREF shows a powered DUT (the caller checks; see diag.py).
from machine import Pin, ADC
import time

PINS = {
    'A': dict(swdio=4, swclk=5, swo=6, nrst_drv=7, nrst_sense=15, uart_tx=16, uart_rx=17,
              vtref_adc=8, en_swd=9, en_uart=46, en_vtref=10),
    'B': dict(swdio=11, swclk=12, swo=14, nrst_drv=21, nrst_sense=47, uart_tx=48, uart_rx=35,
              vtref_adc=1, en_swd=40, en_uart=41, en_vtref=42),
}
LED = 2

VTREF_SETTLE_MS = 50      # divider tau = 5 ms; datasheet-derived minimum is 25 ms
VTREF_DISCHARGE_MS = 80   # after opening: C316 discharges through R316, tau = 10 ms
VTREF_OK_MIN = 2.8        # DUT considered powered (3.3 V I/O design; nRF DK runs at ~3.0 V)
VTREF_OK_MAX = 3.6


def _out_low(n):
    return Pin(n, Pin.OUT, value=0, pull=None)


def safe_all():
    """Every switch open, every driver idle. Safe to call at any time."""
    for ch in PINS.values():
        for k in ('en_swd', 'en_uart', 'en_vtref', 'nrst_drv'):
            _out_low(ch[k])
        Pin(ch['swclk'], Pin.OUT, value=0, pull=None)
        Pin(ch['swdio'], Pin.IN, pull=None)
    # pure inputs: enable their input buffers (GPIO6/14/15 reset with input disabled); not critical, so last
    for ch in PINS.values():
        for k in ('swo', 'nrst_sense', 'uart_rx'):
            try:
                Pin(ch[k], Pin.IN, pull=None)
            except Exception:
                pass


class Channel:
    def __init__(self, name):
        self.name = name
        self.p = PINS[name]
        p = self.p
        self.en_swd = _out_low(p['en_swd'])
        self.en_uart = _out_low(p['en_uart'])
        self.en_vtref = _out_low(p['en_vtref'])
        self.nrst_drv = _out_low(p['nrst_drv'])
        self.nrst_sense = Pin(p['nrst_sense'], Pin.IN, pull=None)
        self.adc = ADC(Pin(p['vtref_adc']), atten=ADC.ATTN_11DB)

    # ---- state -------------------------------------------------------------------------------
    def safe(self):
        """Release reset, open all switch groups (signal groups first, VTref last)."""
        self.nrst_drv(0)
        self.en_swd(0)
        self.en_uart(0)
        self.en_vtref(0)
        Pin(self.p['swclk'], Pin.OUT, value=0, pull=None)
        Pin(self.p['swdio'], Pin.IN, pull=None)

    def state(self):
        return dict(en_vtref=self.en_vtref(), en_swd=self.en_swd(), en_uart=self.en_uart(),
                    nrst_drv=self.nrst_drv())

    # ---- VTref -------------------------------------------------------------------------------
    def adc_volts(self, n=32):
        s = 0
        for _ in range(n):
            s += self.adc.read_uv()
        return s / n / 1e6

    def vtref(self, n=32):
        """DUT VTref in volts (ADC pin x 2). Only meaningful while EN_VTREF is closed."""
        return 2 * self.adc_volts(n)

    def close_vtref(self, settle_ms=VTREF_SETTLE_MS):
        self.en_vtref(1)
        time.sleep_ms(settle_ms)
        return self.vtref()

    def open_vtref(self, discharge_ms=VTREF_DISCHARGE_MS):
        self.en_vtref(0)
        time.sleep_ms(discharge_ms)
        return self.vtref()

    def vtref_ok(self, v=None):
        if v is None:
            v = self.vtref()
        return VTREF_OK_MIN <= v <= VTREF_OK_MAX

    # ---- SWD group -----------------------------------------------------------------------------
    def close_swd(self, force=False):
        """Interlock: requires EN_VTREF closed and VTref in range (unless force=True)."""
        if not force:
            if not self.en_vtref():
                raise RuntimeError('interlock: EN_VTREF is open')
            v = self.vtref()
            if not self.vtref_ok(v):
                raise RuntimeError('interlock: VTref %.2f V out of range' % v)
        # tool side safe before closing: SWCLK low, SWDIO input, reset released
        Pin(self.p['swclk'], Pin.OUT, value=0, pull=None)
        Pin(self.p['swdio'], Pin.IN, pull=None)
        self.nrst_drv(0)
        self.en_swd(1)
        time.sleep_us(10)

    def open_swd(self):
        self.nrst_drv(0)
        self.en_swd(0)

    # ---- raw line helpers (diagnostics) ---------------------------------------------------------
    def read_line(self, key, pull=None, settle_us=2000):
        """Read a tool-side line as an input with an optional internal pull (Pin.PULL_UP/DOWN).
        Leaves it as an input without pull. Do not use on EN pins."""
        assert not key.startswith('en_'), 'never probe EN pins'
        pin = Pin(self.p[key], Pin.IN, pull=pull)
        time.sleep_us(settle_us)
        v = pin.value()
        Pin(self.p[key], Pin.IN, pull=None)
        return v

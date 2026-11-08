# SPDX-License-Identifier: MIT
# main.py - r5 board test firmware: heartbeat on the status LED (GPIO2), REPL stays free.
# Run the SWD board test from the host:  ./run.sh diag A
from machine import Pin, Timer

_led = Pin(2, Pin.OUT, value=0)
_n = [0]


def _beat(_):
    _n[0] = (_n[0] + 1) % 10
    _led(1 if _n[0] == 0 else 0)   # 100 ms flash every second


Timer(0).init(period=100, callback=_beat)
print('r5 board test firmware ready: import diag; diag.run("A")')

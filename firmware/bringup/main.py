# SPDX-License-Identifier: MIT
# Dual SWD Flasher r5 - bring-up blink.
# STATUS LED D101 on GPIO2 (active high, R103 1k). All isolation enables
# (GPIO9/46/10/40/41/42) are left untouched, so their 100k pull-downs keep every switch open.
from machine import Pin
import time
led = Pin(2, Pin.OUT)
print("dual_swd_flasher r5: blinking STATUS LED on GPIO2 (Ctrl-C to stop)")
while True:
    led.value(1); time.sleep_ms(250)
    led.value(0); time.sleep_ms(250)

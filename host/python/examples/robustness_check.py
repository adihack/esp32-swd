#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""esp-swd robustness checks on real hardware (r5 + nRF52840 DK on channel A, test firmware with ESW_TEST_HOOKS).

1. reset-halt: halt at the reset vector (needs C_DEBUGEN), then a normal reset lets the image run again
2. panic with the SWD group closed: the board must come back by itself, isolated, still answering
3. host loss: a client that holds SWD and is attached disappears without releasing; the firmware must end the
   session (debug off, pin reset) and isolate the channel
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from esw import Esw, EswError, proto as P  # noqa: E402
from nrf52_blinky import BLINKY  # noqa: E402

PORT, CH = "/dev/ttyACM0", 0
DHCSR, P0_OUT = 0xE000EDF0, 0x50000504
RESULTS = []


def check(name, ok, info=""):
    RESULTS.append((name, ok))
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, info))


def connect(timeout=15.0):
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        try:
            d = Esw(PORT, timeout=1.0)
            d.ping()
            return d
        except Exception:  # noqa: BLE001 - port re-appearing after a reset
            time.sleep(0.3)
    raise TimeoutError("device did not come back")


def hold_attach(d):
    d.hold(CH, P.GROUP_SWD)
    st = d.wait_group(CH, P.GROUP_SWD, "CLOSED", 2.0)
    return st, d.attach(CH)


def leds(d, n=6):
    out = []
    for _ in range(n):
        v = d.mem_read(CH, P0_OUT, 1)[0]
        out.append(",".join(name for bit, name in ((13, "1"), (14, "2"), (15, "3"), (16, "4")) if not (v >> bit) & 1) or "-")
        time.sleep(0.12)
    return " ".join(out)


print("== 1. reset-halt")
d = connect()
st, t = hold_attach(d)
print("  attached %s, VTref %s mV" % (t["part"], st["vtref_mv"]))
try:
    d.reset(CH, halt=True)
    dh = d.mem_read(CH, DHCSR, 1)[0]
    check("reset-halt halts the core", bool(dh & (1 << 17)), "DHCSR=0x%08X" % dh)
except EswError as e:
    # build code F: APPROTECT re-engages at reset until the image runs; halted at the vector it may stay locked
    check("reset-halt", False, "-> %s" % e)
try:
    d.reset(CH)
    seen = leds(d, 10)
    check("normal reset afterwards, image runs", len(set(seen.split())) >= 2 and "-" not in seen.split(),
          "LEDs " + seen)   # the chase visits >= 2 different LEDs in 1.2 s
except EswError as e:
    check("normal reset afterwards", False, "-> %s" % e)
d.detach(CH)
d.release(CH, P.GROUP_SWD)
d.close()

print("== 2. panic with the SWD group closed")
d = connect()
st, t = hold_attach(d)
before = d.state(CH)
print("  before: swd=%s vtref_closed=%s, attached %s" % (before["swd"], before["vtref_closed"], t["part"]))
up_before = d.info()["uptime_ms"]
d.test_panic()
d.close()
time.sleep(0.5)
d = connect()
info = d.info()
after = d.state(CH)
check("board came back by itself", info["uptime_ms"] < up_before or info["uptime_ms"] < 5000,
      "uptime %d ms, reset reason %d (4 = panic)" % (info["uptime_ms"], info["reset_reason"]))
check("reset reason is PANIC", info["reset_reason"] == 4)
check("channel isolated after the crash", after["swd"] == "OPEN" and not after["vtref_closed"] and not after["holders_swd"],
      str(after))
d.close()

print("== 3. host loss mid-session")
d = connect()
st, t = hold_attach(d)
print("  attached %s; closing the port without release/detach" % t["part"])
d.close()
time.sleep(7.0)   # lease 3 s + grace 2 s + margin
d = connect()
after = d.state(CH)
check("channel isolated after host loss", after["swd"] == "OPEN" and not after["vtref_closed"] and not after["holders_swd"],
      str(after))
try:
    d.mem_read(CH, 0, 1)
    check("session ended (no stale attach)", False, "MEM_READ still worked")
except EswError as e:
    check("session ended (no stale attach)", e.status == -10, "MEM_READ -> %s" % e)
d.close()

print("== 4. REBOOT: invalid mode rejected; normal reboot with SWD held comes back isolated")
d = connect()
try:
    d.request(P.REBOOT, bytes([0xFF]))
    check("REBOOT(0xFF) rejected", False, "accepted")
except EswError as e:
    check("REBOOT(0xFF) rejected", e.status == -1, str(e))
st, t = hold_attach(d)
d.reboot()
d.close()
time.sleep(0.5)
d = connect()
info, after = d.info(), d.state(CH)
check("came back after REBOOT(0)", info["reset_reason"] == 3, "reset reason %d (3 = software)" % info["reset_reason"])
check("channel isolated after REBOOT", after["swd"] == "OPEN" and not after["vtref_closed"] and not after["holders_swd"],
      str(after))
d.close()

print("== 5. CRC verify on the target detects a corrupted word (then the LED image is restored)")
d = connect()
st, t = hold_attach(d)
try:
    data = os.urandom(64 * 1024)
    d.erase(CH, 0, len(data))
    d.program(CH, 0, data)
    check("CRC matches the image", d.crc32(CH, 0, len(data)) == zlib.crc32(data))
    d.program(CH, 0x8000, b"\0\0\0\0")   # NOR: clear one word's bits without an erase
    check("CRC detects one corrupted word", d.crc32(CH, 0, len(data)) != zlib.crc32(data))
finally:
    image = struct.pack("<16I", *BLINKY)   # never leave random bytes as firmware: they run after the next reset
    d.erase(CH, 0, 4096)
    d.program(CH, 0, image)
    d.reset(CH)
    d.detach(CH)
    d.release(CH, P.GROUP_SWD)
    d.close()

print("\n%d/%d checks passed" % (sum(ok for _, ok in RESULTS), len(RESULTS)))
sys.exit(0 if all(ok for _, ok in RESULTS) else 1)

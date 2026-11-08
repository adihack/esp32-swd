# SPDX-License-Identifier: MIT
# nrf52.py - nRF52840 specifics on top of swd.SWD (identification, APPROTECT status, halt test, recover).
import time

DPIDR_NRF52 = 0x2BA01477      # Cortex-M4 SW-DP (ADIv5, DPv1)
AHB_AP = 0
CTRL_AP = 1
AHB_AP_IDR = 0x24770011
CTRL_AP_IDR = 0x02880000      # Nordic CTRL-AP

CTRLAP_RESET = 0x000
CTRLAP_ERASEALL = 0x004
CTRLAP_ERASEALLSTATUS = 0x008
CTRLAP_APPROTECTSTATUS = 0x00C

CPUID = 0xE000ED00
DHCSR = 0xE000EDF0
DCRSR = 0xE000EDF4
DCRDR = 0xE000EDF8
DBGKEY = 0xA05F0000

FICR = [  # (name, address, formatter)
    ('CODEPAGESIZE', 0x10000010, 'dec'),
    ('CODESIZE (pages)', 0x10000014, 'dec'),
    ('DEVICEID[0]', 0x10000060, 'hex'),
    ('DEVICEID[1]', 0x10000064, 'hex'),
    ('DEVICEADDRTYPE', 0x100000A0, 'hex'),
    ('DEVICEADDR[0]', 0x100000A4, 'hex'),
    ('DEVICEADDR[1]', 0x100000A8, 'hex'),
    ('INFO.PART', 0x10000100, 'hex'),
    ('INFO.VARIANT', 0x10000104, 'ascii'),
    ('INFO.PACKAGE', 0x10000108, 'hex'),
    ('INFO.RAM (KB)', 0x1000010C, 'hexdec'),
    ('INFO.FLASH (KB)', 0x10000110, 'hexdec'),
]
UICR = [
    ('UICR.PSELRESET[0]', 0x10001200, 'hex'),
    ('UICR.PSELRESET[1]', 0x10001204, 'hex'),
    ('UICR.APPROTECT', 0x10001208, 'hex'),
    ('UICR.REGOUT0', 0x10001304, 'hex'),
]


def fmt(v, kind):
    if kind == 'dec':
        return '%d' % v
    if kind == 'ascii':
        s = ''.join(chr((v >> s) & 0xFF) for s in (24, 16, 8, 0))
        return '0x%08X "%s"' % (v, s if all(32 <= ord(c) < 127 for c in s) else '?')
    if kind == 'hexdec':
        return '0x%X (%d)' % (v, v)
    return '0x%08X' % v


def ap_scan(swd, n=2):
    return [(ap, swd.ap_read(ap, 0xFC)) for ap in range(n)]


def approtect_open(swd):
    """True if CTRL-AP APPROTECTSTATUS says debug access is allowed."""
    return bool(swd.ap_read(CTRL_AP, CTRLAP_APPROTECTSTATUS) & 1)


def read_table(swd, table):
    out = []
    for name, addr, kind in table:
        out.append((name, addr, swd.read32(addr), kind))
    return out


def halt_test(swd):
    """Halt the core, read PC, resume, drop C_DEBUGEN. Returns (pc, dhcsr_halted, dhcsr_after)."""
    swd.write32(DHCSR, DBGKEY | 0x3)          # C_DEBUGEN | C_HALT
    t0 = time.ticks_ms()
    while True:
        d = swd.read32(DHCSR)
        if d & (1 << 17):                       # S_HALT
            break
        if time.ticks_diff(time.ticks_ms(), t0) > 100:
            raise RuntimeError('core did not halt, DHCSR=0x%08X' % d)
    swd.write32(DCRSR, 15)                     # read R15 (PC)
    while not (swd.read32(DHCSR) & (1 << 16)):  # S_REGRDY
        pass
    pc = swd.read32(DCRDR)
    swd.write32(DHCSR, DBGKEY | 0x1)           # resume, debug still enabled
    swd.write32(DHCSR, DBGKEY)                 # C_DEBUGEN off
    after = swd.read32(DHCSR)
    return pc, d, after


NVMC_READY = 0x4001E400
NVMC_CONFIG = 0x4001E504
UICR_APPROTECT = 0x10001208
UICR_APPROTECT_HWDISABLED = 0xFFFFFF5A


def erase_all(swd, timeout_ms=15000):
    """CTRL-AP ERASEALL: erases flash, RAM and UICR. DESTRUCTIVE. No reset afterwards: on current nRF52840
    revisions APPROTECT re-engages at the next reset unless UICR and the firmware both disable it, so the
    debugger has to keep working in the same session (Nordic: no reset between ERASEALL and reflash)."""
    swd.ap_write(CTRL_AP, CTRLAP_ERASEALL, 1)
    t0 = time.ticks_ms()
    while swd.ap_read(CTRL_AP, CTRLAP_ERASEALLSTATUS) & 1:
        if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
            raise RuntimeError('ERASEALL timeout')
        time.sleep_ms(50)
    swd.ap_write(CTRL_AP, CTRLAP_ERASEALL, 0)
    return time.ticks_diff(time.ticks_ms(), t0)


NVMC_ERASEPAGE = 0x4001E508
UICR_PSELRESET = (0x10001200, 0x10001204)
PSELRESET_P0_18 = 0x00000012      # PIN 18, PORT 0, CONNECT = 0 (connected): the DK reset button / nRESET
P0_OUT = 0x50000504
DK_LEDS = ((13, 'LED1'), (14, 'LED2'), (15, 'LED3'), (16, 'LED4'))   # active low


def nvmc_wait(swd, timeout_ms=500):
    t0 = time.ticks_ms()
    while not (swd.read32(NVMC_READY) & 1):
        if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
            raise RuntimeError('NVMC busy timeout')


def nvmc_erase_page(swd, addr):
    swd.write32(NVMC_CONFIG, 2)                 # EEN
    swd.write32(NVMC_ERASEPAGE, addr)
    nvmc_wait(swd)
    swd.write32(NVMC_CONFIG, 0)


def nvmc_program(swd, addr, words):
    swd.write32(NVMC_CONFIG, 1)                 # WEN
    for i, w in enumerate(words):
        swd.write32(addr + 4 * i, w)
        nvmc_wait(swd)
    swd.write32(NVMC_CONFIG, 0)                 # REN


def nvmc_write32(swd, addr, val):
    nvmc_program(swd, addr, [val])
    return swd.read32(addr)


def halt(swd, timeout_ms=100):
    swd.write32(DHCSR, DBGKEY | 0x3)
    t0 = time.ticks_ms()
    while True:
        d = swd.read32(DHCSR)
        if d & (1 << 17):
            return d
        if time.ticks_diff(time.ticks_ms(), t0) > timeout_ms:
            raise RuntimeError('core did not halt, DHCSR=0x%08X' % d)


def ctrlap_reset(swd, hold_ms=5):
    """System reset through the CTRL-AP. Works without a pin-reset configuration in UICR."""
    swd.ap_write(CTRL_AP, CTRLAP_RESET, 1)
    time.sleep_ms(hold_ms)
    swd.ap_write(CTRL_AP, CTRLAP_RESET, 0)


def leds_on(out):
    return [name for bit, name in DK_LEDS if not (out >> bit) & 1]

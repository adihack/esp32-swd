# SPDX-License-Identifier: MIT
# diag.py - r5 board test: staged SWD connection diagnostics against an nRF52840 DK.
# Every stage proves one link of the chain before the next stage relies on it, so a failure points at
# a specific part (resistor, switch, cable, DK setting). The channel always ends in the safe state.
#
#   import diag; diag.run('A')        full test (resets the DUT once, halts its core for ~1 ms)
#   diag.levels('A')                  print EN states, tool-side line levels and the VTref ADC
#   diag.hold_vtref('A')              close EN_VTREF and leave it closed (for meter checks); diag.release('A')
#   diag.recover('A', really=True)    nRF52 CTRL-AP ERASEALL (DESTRUCTIVE: erases the DUT, clears APPROTECT)
import time
from machine import Pin
import r5
import swd as swdmod
import nrf52


class Abort(Exception):
    pass


class Report:
    def __init__(self):
        self.n = {'PASS': 0, 'WARN': 0, 'FAIL': 0, 'INFO': 0}

    def _p(self, tag, sid, msg, hints):
        self.n[tag] += 1
        print('[%s] %-4s %s' % (tag, sid, msg))
        for h in hints:
            print('              -> ' + h)

    def ok(self, sid, msg, *hints):
        self._p('PASS', sid, msg, hints)

    def warn(self, sid, msg, *hints):
        self._p('WARN', sid, msg, hints)

    def fail(self, sid, msg, *hints):
        self._p('FAIL', sid, msg, hints)

    def info(self, sid, msg, *hints):
        self._p('INFO', sid, msg, hints)

    def stage(self, title):
        print('\n== %s' % title)


def _ref(ch, name):
    """Part designator: channel A uses 3xx, channel B 4xx."""
    return name.replace('x', '3' if ch.name == 'A' else '4')


def _check_vtref(ch, rep, sid):
    v = ch.vtref()
    if not ch.vtref_ok(v):
        rep.fail(sid, 'VTref dropped to %.2f V while connected: opening switches' % v,
                 'DUT lost power, or the VTref contact is intermittent (cable).')
        raise Abort()
    return v


def run(name='A', halt=True, swd_half_period_us=(0, 20, 200)):
    r5.safe_all()
    ch = r5.Channel(name)
    rep = Report()
    link = None
    connected = False
    print('r5 board test, channel %s: SWDIO=GPIO%d SWCLK=GPIO%d NRST_DRV=GPIO%d NRST_SENSE=GPIO%d '
          'VTREF_ADC=GPIO%d EN_SWD=GPIO%d EN_VTREF=GPIO%d'
          % (name, ch.p['swdio'], ch.p['swclk'], ch.p['nrst_drv'], ch.p['nrst_sense'],
             ch.p['vtref_adc'], ch.p['en_swd'], ch.p['en_vtref']))
    try:
        # ------------------------------------------------------------------------------------------
        rep.stage('Stage 0: board self-test (all switches open, DUT not involved)')
        st = ch.state()
        if any(st.values()):
            rep.fail('0.1', 'EN/driver pins not low after safe state: %s' % st)
            raise Abort()
        rep.ok('0.1', 'EN_VTREF, EN_SWD, EN_UART, NRST_DRV driven low (all switches open)')

        v = ch.vtref()
        if v / 2 < 0.15:
            rep.ok('0.2', 'VTref ADC pin %.3f V with EN_VTREF open (pulled to 0 by %s)' % (v / 2, _ref(ch, 'Rx16')))
        else:
            rep.fail('0.2', 'VTref ADC pin %.3f V with EN_VTREF open, expected ~0' % (v / 2),
                     '%s missing, or the VTref switch (%s sw3) is leaking/closed.' % (_ref(ch, 'Rx16'), _ref(ch, 'Ux02')),
                     'Check TP%s (EN_VTREF) is 0 V.' % _ref(ch, 'x05'))

        expect = [('swdio', 1, 'Rx06 pull-up / Rx05 33R'), ('swclk', 0, 'Rx07 pull-down'),
                  ('swo', 1, 'Rx09 pull-up'), ('nrst_sense', 1, 'Rx08 pull-up / Qx01 off'),
                  ('uart_rx', 1, 'Rx10 pull-up')]
        bad = []
        for key, exp, parts in expect:
            got = ch.read_line(key)
            if got != exp:
                bad.append('%s=%d (expected %d: check %s)' % (key.upper(), got, exp, _ref(ch, parts)))
        Pin(ch.p['swclk'], Pin.OUT, value=0, pull=None)
        if bad:
            rep.fail('0.3', 'tool-side idle levels wrong: ' + '; '.join(bad))
        else:
            rep.ok('0.3', 'tool-side idle levels: SWDIO=1 SWCLK=0 SWO=1 NRST_SENSE=1 UART_RX=1')

        ch.nrst_drv(1)
        time.sleep_ms(2)
        a = ch.nrst_sense()
        ch.nrst_drv(0)
        time.sleep_ms(2)
        b = ch.nrst_sense()
        if (a, b) == (0, 1):
            rep.ok('0.4', 'nRESET driver: DRV=1 -> SENSE=0, DRV=0 -> SENSE=1 (%s works, switch open)' % _ref(ch, 'Qx01'))
        else:
            rep.fail('0.4', 'nRESET driver: DRV=1 -> SENSE=%d, DRV=0 -> SENSE=%d (expected 0, 1)' % (a, b),
                     'Check %s (2N7002), %s gate pull-down, %s sense pull-up.'
                     % (_ref(ch, 'Qx01'), _ref(ch, 'Rx12'), _ref(ch, 'Rx08')))

        probes = [('swclk', Pin.PULL_UP, 'SWCLK', 'pull-down'),
                  ('swdio', Pin.PULL_DOWN, 'SWDIO', 'pull-up'),
                  ('nrst_sense', Pin.PULL_DOWN, 'nRESET', 'pull-up')]
        base = {}
        for key, pull, _, _ in probes:
            base[key] = ch.read_line(key, pull)
        Pin(ch.p['swclk'], Pin.OUT, value=0, pull=None)
        rep.info('0.5', 'pull-probe baseline (45k internal pull vs 100k external): '
                 'SWCLK+pu=%d SWDIO+pd=%d NRST+pd=%d (expected 1 0 0)'
                 % (base['swclk'], base['swdio'], base['nrst_sense']))

        # ------------------------------------------------------------------------------------------
        rep.stage('Stage 1: DUT power via VTref (EN_VTREF closed)')
        v = ch.close_vtref()
        if v < 0.3:
            rep.fail('1.1', 'VTref %.2f V: no DUT supply seen' % v,
                     'Is the DK switched on? Cable on the DK "Debug in" connector (P18), pin 1 to pin 1?',
                     'Meter check: run diag.hold_vtref(\'%s\'), then measure J%s pin 1 and TP%s (tool side).'
                     % (name, _ref(ch, 'x01'), _ref(ch, 'x01')),
                     'J pin 1 OK but TP 0 V -> %s sw3 or EN_VTREF (TP%s should be 3.3 V).'
                     % (_ref(ch, 'Ux02'), _ref(ch, 'x05')))
            raise Abort()
        if v < r5.VTREF_OK_MIN:
            rep.fail('1.1', 'VTref %.2f V is below %.1f V: DUT I/O voltage does not match the 3.3 V design' % (v, r5.VTREF_OK_MIN),
                     'Not closing the signal switches: 3.3 V drive would back-power / overdrive the DUT.')
            raise Abort()
        if v > r5.VTREF_OK_MAX:
            rep.fail('1.1', 'VTref %.2f V is above %.1f V: wrong pin or DUT supply' % (v, r5.VTREF_OK_MAX))
            raise Abort()
        rep.ok('1.1', 'VTref %.2f V (ADC pin %.3f V): DUT powered' % (v, v / 2))
        if v < 3.25:
            rep.warn('1.2', 'DUT VDD %.2f V < 3.3 V: tool highs are %.2f V above DUT VDD (nRF52840 abs max VDD+0.3 V)'
                     % (v, 3.3 - v),
                     'Acceptable for a short board test (33R + DRIVE_0 limit clamp current).',
                     'Production: run DUTs at 3.3 V or add VTref-tracking I/O (rev E topic).')
        samples = [ch.vtref(8) for _ in range(5)]
        rep.info('1.3', 'VTref noise: min %.3f max %.3f V' % (min(samples), max(samples)))

        # ------------------------------------------------------------------------------------------
        rep.stage('Stage 2: close EN_SWD, check the lines on the DUT side')
        ch.close_swd()
        time.sleep_ms(2)
        v2 = _check_vtref(ch, rep, '2.1')
        rep.ok('2.1', 'EN_SWD closed (interlock passed), VTref now %.2f V (delta %+.3f V)' % (v2, v2 - v))

        lv = {k: ch.read_line(k) for k in ('swdio', 'swclk', 'swo', 'nrst_sense')}
        Pin(ch.p['swclk'], Pin.OUT, value=0, pull=None)
        rep.info('2.2', 'idle levels with switch closed: SWDIO=%d SWCLK=%d SWO=%d NRST_SENSE=%d'
                 % (lv['swdio'], lv['swclk'], lv['swo'], lv['nrst_sense']))
        if lv['swdio'] == 0 and lv['swo'] == 0:
            rep.fail('2.2', 'SWDIO and SWO both held low on the DUT side',
                     'Cable plugged 180 degrees rotated? (puts SWDIO on GND pin 9, SWO on GND pin 5, nRESET on VTref).')
            raise Abort()
        if lv['swdio'] == 0:
            rep.fail('2.2', 'SWDIO held low on the DUT side (short to GND or rotated cable)')
            raise Abort()
        if lv['swclk'] == 1:
            rep.fail('2.2', 'SWCLK pulled high on the DUT side: another debugger drives it',
                     'nRF52840 DK: set SW6 to "nRF ONLY" (or unplug the J-Link USB and power via nRF USB).')
            raise Abort()
        if lv['swo'] == 0:
            rep.warn('2.2', 'SWO held low on the DUT side (DUT uses P1.00 as an output?)')
        if lv['nrst_sense'] == 0:
            rep.warn('2.2', 'nRESET held low on the DUT side (reset button, DK interface MCU, or DUT brown-out)')

        seen = 0
        for key, pull, label, dutpull in probes:
            got = ch.read_line(key, pull)
            if base[key] != (1 if pull == Pin.PULL_UP else 0):
                rep.info('2.3', '%s probe inconclusive (baseline %d)' % (label, base[key]))
            elif got != base[key]:
                seen += 1
                rep.ok('2.3', '%s reaches the DUT: its internal %s is visible (%d -> %d)' % (label, dutpull, base[key], got))
            else:
                rep.warn('2.3', '%s: no DUT %s seen (%d -> %d)' % (label, dutpull, base[key], got),
                         'Wire not connected, or this DUT pin has no pull (nRESET: pin reset not enabled).')
        Pin(ch.p['swclk'], Pin.OUT, value=0, pull=None)
        if seen == 0:
            rep.warn('2.3', 'no DUT pull seen on any line: EN_SWD switch (%s) not closing, or cable not connected'
                     % _ref(ch, 'Ux01'), 'Check TP%s (EN_SWD) = 3.3 V now.' % _ref(ch, 'x06'))

        # ------------------------------------------------------------------------------------------
        rep.stage('Stage 3: nRESET through to the DUT (the DK restarts once - watch its LEDs)')
        ch.nrst_drv(1)
        time.sleep_ms(20)
        a = ch.nrst_sense()
        t0 = time.ticks_us()
        ch.nrst_drv(0)
        rise = None
        while time.ticks_diff(time.ticks_us(), t0) < 100000:
            if ch.nrst_sense():
                rise = time.ticks_diff(time.ticks_us(), t0)
                break
        if a == 0 and rise is not None:
            rep.ok('3.1', 'nRESET asserted (SENSE=0), released, line high again after %d us' % rise)
        elif a != 0:
            rep.fail('3.1', 'nRESET asserted but SENSE stayed high (%s or the DUT side shorted high?)' % _ref(ch, 'Qx01'))
        else:
            rep.warn('3.1', 'nRESET still low 100 ms after release: DUT (or DK) keeps reset asserted')
        time.sleep_ms(50)
        _check_vtref(ch, rep, '3.2')

        # ------------------------------------------------------------------------------------------
        rep.stage('Stage 4: SWD link (DPIDR)')
        dpidr = None
        last = None
        for hp in swd_half_period_us:
            link = swdmod.SWD(ch.p['swclk'], ch.p['swdio'], half_period_us=hp)
            try:
                dpidr = link.connect()
                rep.ok('4.1', 'DPIDR 0x%08X read with half-period %d us (designer 0x%03X, DPv%d, partno 0x%02X)'
                       % (dpidr, hp, (dpidr >> 1) & 0x7FF, (dpidr >> 12) & 0xF, (dpidr >> 20) & 0xFF))
                break
            except swdmod.SWDError as e:
                rep.warn('4.1', 'half-period %d us: %s' % (hp, e))
                last = e
                link.end()
        if dpidr is None:
            ack = last.ack if last else None
            hints = []
            if ack == 7:
                hints = ['Target never drove SWDIO. Likely: SWCLK not reaching the target, cable on DK "Debug out"',
                         'instead of "Debug in" (P18), DK onboard J-Link still active (SW6 -> nRF ONLY),',
                         'or GND missing between the boards (J pins 3/5/9).']
            elif ack == 0:
                hints = ['SWDIO stuck low during ACK: short or contention on SWDIO.']
            else:
                hints = ['Garbled ACK: signal integrity. Shorter cable, check GND, try a single cable with no adapters.']
            rep.fail('4.1', 'no valid SWD response at any speed', *hints)
            raise Abort()
        connected = True
        if dpidr == nrf52.DPIDR_NRF52:
            rep.ok('4.2', 'DPIDR matches nRF52 (Cortex-M4 SW-DP 0x%08X)' % nrf52.DPIDR_NRF52)
        else:
            rep.warn('4.2', 'DPIDR is not the nRF52 value 0x%08X (nRF5340 = 0x6BA02477)' % nrf52.DPIDR_NRF52)
        rep.info('4.3', 'bit-bang SWCLK ~%d Hz' % link.measure_hz())
        errs = 0
        for _ in range(50):
            try:
                if link.dp_read(swdmod.DP_DPIDR) != dpidr:
                    errs += 1
            except swdmod.SWDError:
                errs += 1
                link.connect()
        if errs:
            rep.warn('4.4', '%d/50 DPIDR re-reads failed (marginal link)' % errs)
        else:
            rep.ok('4.4', '50/50 DPIDR re-reads clean')

        # ------------------------------------------------------------------------------------------
        rep.stage('Stage 5: debug power-up, access ports')
        cs = link.power_up()
        rep.ok('5.1', 'CDBGPWRUPACK/CSYSPWRUPACK set, CTRL/STAT 0x%08X' % cs)
        unlocked = nrf52.approtect_open(link)
        for ap, idr in nrf52.ap_scan(link):
            exp = {0: nrf52.AHB_AP_IDR, 1: nrf52.CTRL_AP_IDR}[ap]
            what = {0: 'AHB-AP', 1: 'Nordic CTRL-AP'}[ap]
            if idr == exp:
                rep.ok('5.2', 'AP%d IDR 0x%08X (%s)' % (ap, idr, what))
            elif ap == 0 and not unlocked:
                rep.info('5.2', 'AP0 IDR 0x%08X: AHB-AP gated by APPROTECT (0x%08X once unlocked)' % (idr, exp))
            else:
                rep.warn('5.2', 'AP%d IDR 0x%08X (%s expects 0x%08X)' % (ap, idr, what, exp))
        if unlocked:
            rep.ok('5.3', 'CTRL-AP APPROTECTSTATUS: debug access allowed')
        else:
            rep.warn('5.3', 'APPROTECT enabled: CPU and memory not accessible (DP and CTRL-AP are)',
                     'diag.recover(\'%s\', really=True) erases the whole chip and unlocks it (DESTRUCTIVE).' % name)
        _check_vtref(ch, rep, '5.4')

        # ------------------------------------------------------------------------------------------
        if unlocked:
            rep.stage('Stage 6: memory access through the AHB-AP')
            csw = link.mem_init()
            rep.info('6.1', 'AHB-AP CSW 0x%08X (DeviceEn=%d)' % (csw, (csw >> 6) & 1))
            cpuid = link.read32(nrf52.CPUID)
            (rep.ok if cpuid == 0x410FC241 else rep.warn)('6.2', 'CPUID 0x%08X (Cortex-M4 r0p1 = 0x410FC241)' % cpuid)
            ficr = nrf52.read_table(link, nrf52.FICR)
            for nm, addr, val, kind in ficr:
                print('              %-18s @0x%08X = %s' % (nm, addr, nrf52.fmt(val, kind)))
            part = [v for nm, a, v, k in ficr if nm == 'INFO.PART'][0]
            (rep.ok if part == 0x52840 else rep.warn)('6.3', 'FICR INFO.PART 0x%X' % part)
            for nm, addr, val, kind in nrf52.read_table(link, nrf52.UICR):
                print('              %-18s @0x%08X = %s' % (nm, addr, nrf52.fmt(val, kind)))
            sp, rv = link.read32(0), link.read32(4)
            rep.info('6.4', 'flash[0..7]: initial SP 0x%08X, reset vector 0x%08X%s'
                     % (sp, rv, ' (erased)' if sp == 0xFFFFFFFF else ''))
            if halt:
                pc, d, after = nrf52.halt_test(link)
                rep.ok('6.5', 'core halted (DHCSR 0x%08X), PC=0x%08X, resumed (DHCSR 0x%08X)' % (d, pc, after))
            _check_vtref(ch, rep, '6.6')

    except Abort:
        pass
    except Exception as e:  # keep going to the teardown no matter what
        rep.fail('!!', 'unexpected %s: %s' % (type(e).__name__, e))
    finally:
        rep.stage('Stage 7: disconnect (debug off, pin reset, open switches)')
        try:
            if connected and link is not None:
                try:
                    link.power_down()
                except Exception as e:
                    rep.warn('7.1', 'debug power-down failed: %s' % e)
                link.end()
                if ch.en_swd() and ch.vtref_ok():
                    ch.nrst_drv(1)          # nRF52 leaves debug interface mode only on a reset
                    time.sleep_ms(10)
                    ch.nrst_drv(0)
                    time.sleep_ms(10)
                    rep.ok('7.1', 'debug power-down + pin reset done')
        finally:
            ch.safe()
            r5.safe_all()
        time.sleep_ms(r5.VTREF_DISCHARGE_MS)
        v = ch.vtref()
        (rep.ok if v / 2 < 0.15 else rep.warn)('7.2', 'all switches open, VTref ADC pin %.3f V' % (v / 2))
        n = rep.n
        verdict = 'PASS' if n['FAIL'] == 0 and n['WARN'] == 0 else ('FAIL' if n['FAIL'] else 'PASS with warnings')
        print('\nRESULT channel %s: %s (%d pass, %d warn, %d fail)' % (name, verdict, n['PASS'], n['WARN'], n['FAIL']))
    return rep.n


# ---- manual helpers --------------------------------------------------------------------------------
def levels(name='A'):
    """Print the current state. Outputs are read back as they are (Pin(n) alone does not re-init); the pure
    inputs are (re)configured as inputs first: GPIO6/15 come out of reset with the input buffer disabled."""
    from machine import ADC
    p = r5.PINS[name]
    for k in ('swo', 'nrst_sense', 'uart_rx'):
        Pin(p[k], Pin.IN, pull=None)
    rd = lambda keys: tuple(Pin(p[k]).value() for k in keys)
    print('EN_VTREF=%d EN_SWD=%d EN_UART=%d NRST_DRV=%d' % rd(('en_vtref', 'en_swd', 'en_uart', 'nrst_drv')))
    print('SWDIO=%d SWCLK=%d SWO=%d NRST_SENSE=%d UART_RX=%d' % rd(('swdio', 'swclk', 'swo', 'nrst_sense', 'uart_rx')))
    adc = ADC(Pin(p['vtref_adc']), atten=ADC.ATTN_11DB)
    uv = sum(adc.read_uv() for _ in range(16)) / 16
    print('VTref ADC pin %.3f V -> VTref %.2f V (valid only with EN_VTREF closed)' % (uv / 1e6, 2 * uv / 1e6))


def hold_vtref(name='A'):
    ch = r5.Channel(name)
    v = ch.close_vtref()
    print('EN_VTREF %s closed and left closed: VTref %.2f V. The DUT supply now carries ~16.5 uA. '
          'diag.release(\'%s\') opens it.' % (name, v, name))


def release(name='A'):
    r5.Channel(name).safe()
    print('channel %s safe: all switches open' % name)


def recover(name='A', really=False):
    """nRF52840 unlock: ERASEALL, UICR.APPROTECT = HwDisabled, then identify the chip in the same debug session.
    DESTRUCTIVE (flash, RAM, UICR). No reset at the end: a reset before new firmware is flashed re-locks
    current nRF52840 revisions (the firmware itself must also write APPROTECT.DISABLE at every boot)."""
    if not really:
        print('recover() erases the whole DUT (flash, RAM, UICR). Call recover(\'%s\', really=True).' % name)
        return
    r5.safe_all()
    ch = r5.Channel(name)
    link = None
    try:
        v = ch.close_vtref()
        if not ch.vtref_ok(v):
            print('VTref %.2f V out of range, abort' % v)
            return
        ch.close_swd()
        link = swdmod.SWD(ch.p['swclk'], ch.p['swdio'])
        print('DPIDR 0x%08X' % link.connect())
        link.power_up()
        print('APPROTECT open before: %s' % nrf52.approtect_open(link))
        print('ERASEALL ...')
        print('ERASEALL done in %d ms' % nrf52.erase_all(link))
        if not nrf52.approtect_open(link):
            print('still locked after ERASEALL (no reset done) - stop here and report')
            return
        print('APPROTECT open after ERASEALL')
        print('AP0 IDR 0x%08X, AHB-AP CSW 0x%08X' % (link.ap_read(0, 0xFC), link.mem_init()))
        print('CPUID 0x%08X' % link.read32(nrf52.CPUID))
        for nm, addr, val, kind in nrf52.read_table(link, nrf52.FICR):
            print('  %-18s @0x%08X = %s' % (nm, addr, nrf52.fmt(val, kind)))
        print('flash[0] 0x%08X (0xFFFFFFFF = erased)' % link.read32(0))
        got = nrf52.nvmc_write32(link, nrf52.UICR_APPROTECT, nrf52.UICR_APPROTECT_HWDISABLED)
        print('UICR.APPROTECT written: 0x%08X (HwDisabled = 0x%08X)' % (got, nrf52.UICR_APPROTECT_HWDISABLED))
        print('Chip is erased and open. Do not reset it before flashing new firmware.')
    finally:
        if link is not None:
            try:
                link.end()
            except Exception:
                pass
        ch.safe()          # releases nRESET (never asserted here) and opens all switches
        r5.safe_all()


def flash_blinky(name='A', erase_if_locked=False):
    """Program the 64-byte nRF52840 DK test image (host/mkblinky.py) over SWD, reset, and prove it runs:
    APPROTECT stays open after reset (the image disables it) and the LED bits in P0.OUT change."""
    import nrf_blinky
    words = nrf_blinky.WORDS
    r5.safe_all()
    ch = r5.Channel(name)
    link = None
    try:
        v = ch.close_vtref()
        if not ch.vtref_ok(v):
            print('VTref %.2f V out of range, abort' % v)
            return
        ch.close_swd()
        link = swdmod.SWD(ch.p['swclk'], ch.p['swdio'])
        print('DPIDR 0x%08X, VTref %.2f V' % (link.connect(), v))
        link.power_up()
        if not nrf52.approtect_open(link):
            if not erase_if_locked:
                print('APPROTECT is on (the DK was reset since the unlock). Rerun with erase_if_locked=True.')
                return
            print('APPROTECT on -> ERASEALL (%d ms)' % nrf52.erase_all(link))
            if not nrf52.approtect_open(link):
                print('still locked after ERASEALL, abort')
                return
        link.mem_init()
        print('core halted, DHCSR 0x%08X' % nrf52.halt(link))

        cur = [link.read32(4 * i) for i in range(len(words))]
        if cur == words:
            print('flash already holds the image')
        else:
            if any(w != 0xFFFFFFFF for w in cur):
                nrf52.nvmc_erase_page(link, 0)
                print('page 0 erased')
            t0 = time.ticks_ms()
            nrf52.nvmc_program(link, 0, words)
            print('programmed %d words in %d ms' % (len(words), time.ticks_diff(time.ticks_ms(), t0)))
        bad = [i for i, w in enumerate(words) if link.read32(4 * i) != w]
        if bad:
            print('VERIFY FAILED at words %s' % bad)
            return
        print('verify OK (%d bytes)' % (4 * len(words)))

        ap = link.read32(nrf52.UICR_APPROTECT)
        if ap == 0xFFFFFFFF:
            ap = nrf52.nvmc_write32(link, nrf52.UICR_APPROTECT, nrf52.UICR_APPROTECT_HWDISABLED)
        print('UICR.APPROTECT 0x%08X' % ap)
        for addr in nrf52.UICR_PSELRESET:
            ps = link.read32(addr)
            if ps == 0xFFFFFFFF:
                ps = nrf52.nvmc_write32(link, addr, nrf52.PSELRESET_P0_18)
            print('UICR.PSELRESET @0x%08X = 0x%08X (P0.18 = 0x12)' % (addr, ps))

        link.write32(nrf52.DHCSR, nrf52.DBGKEY)      # resume, C_DEBUGEN off
        nrf52.ctrlap_reset(link)
        print('CTRL-AP reset -> image starts (watch LED1..LED4 on the DK)')
        time.sleep_ms(200)

        print('reconnect: DPIDR 0x%08X' % link.connect())
        link.power_up()
        if not nrf52.approtect_open(link):
            print('APPROTECT is ON after reset: the image did not run or did not unlock')
            return
        print('APPROTECT open after reset: the image ran its first instruction')
        link.mem_init()
        seen = []
        for _ in range(8):
            out = link.read32(nrf52.P0_OUT)
            seen.append(','.join(nrf52.leds_on(out)) or '-')
            time.sleep_ms(120)
        print('LEDs on, sampled every 120 ms: %s' % ' '.join(seen))
        pc, d, after = nrf52.halt_test(link)
        print('halt test: PC=0x%08X (image code is 0x08..0x2F), resumed' % pc)
        link.power_down()
        link.end()
        ch.nrst_drv(1)                               # pin reset (PSELRESET active now): leave debug mode
        time.sleep_ms(10)
        ch.nrst_drv(0)
        print('pin reset done: DK runs the image without the debugger')
    finally:
        if link is not None:
            try:
                link.end()
            except Exception:
                pass
        ch.safe()
        r5.safe_all()

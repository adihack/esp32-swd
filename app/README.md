# esp-swd firmware (ESP-IDF, board r5)

This is a test firmware for milestone M1 (preview). It runs the portable library (`../lib`) on r5 hardware:
- the r5 board layer (`../boards/r5`) and the ESP32-S3 port (`../ports/esp32s3`);
- the framed protocol (`lib/proto`) over the ESP32-S3 **USB-Serial-JTAG** port (VID:PID `303a:1001`);
- the Python host library and example in `../host/python`.

## Build and flash
```
. ~/esp/esp-idf-v6.0.3/export.sh
export PATH="$PATH:$PWD/../.venv/bin"     # cmake + ninja from the repo venv; appended so IDF's Python stays first
idf.py build
idf.py -p /dev/ttyACM0 flash               # no button needed: USB-Serial-JTAG resets into the ROM downloader in hardware
```
Then run the example:
`../.venv/bin/python ../host/python/examples/nrf52_blinky.py --channel A [--allow-erase]`.

## SWD engines
| PHY | `attach(ch, clock, phy)` | Speed |
|---|---|---|
| 1, dedicated GPIO (default in the Python client) | clock in Hz, 0 = 8 MHz, capped at 10 MHz | 7.4 µs per packet at 8 MHz |
| 0, portable bit-bang | clock = half-period busy loops | 31 µs per packet |

Each channel has its own worker task and dgpio engine, so **both channels run at the same time**:
| Task | Core | Job |
|---|---|---|
| `esw_proto` (priority 5) | 1 | reads USB frames; answers device, state and UART commands; queues channel commands |
| `esw_swd_a` (4) | 1 | channel A: hold/release and every SWD/target command, in order; session supervision |
| `esw_swd_b` (5) | 0 | channel B, the same; shares core 0 with the interlock tick, the UART task and the USB interrupt |

- Dedicated GPIO is per CPU core: each worker creates its channel's engine on its own core at boot and is the only
  user. Every attach re-routes SWCLK/SWDIO to the dedicated signals and self-checks: the board's pin setup runs
  `gpio_config`, which re-routes the pads to plain GPIO.
- Each channel has a queue of 4 request frames (`ESW_PROTO_CH_QUEUE`, protocol 4); the host keeps at most 4 channel
  frames in flight per channel. Responses of the two channels may overtake each other (matched by sequence number).
- **Put the slower, bigger-flash target on channel A**: core 1 is the quiet one. Even a flash-bound target (STM32U5:
  the flash stalls the bus and the engine polls WAIT) loses time when its core is interrupted.
- A worker busy back to back gives its core's idle task 1 ms every 100 ms (the task watchdog watches core 0's).

- Below about 480 kHz interrupts stay enabled during packets. A wait that ends more than 16 cycles late restarts the
  schedule from now, so an interrupt stretches one clock phase and never produces catch-up bursts.
- The board's USB receive ring is 8 KB with no flow control. Firmware before protocol 4 handled frames in place, so
  the host kept at most 3 frames in flight in total; now the dispatcher moves channel frames into their queue at
  once, and the host's limit is per channel (`Esw.CH_BUDGET`, plus a 7 KB budget for everything else).

Benchmarks:
- `host/python/examples/swd_bench.py --phy 1 --clocks 1e6,4e6,8e6`: on the device, no USB in the loop.
- `flash_speed.py --size 1M --window 3 --phy 1 --verify crc`: full flow.

## Target modules
| Module | Parts | Notes |
|---|---|---|
| `nrf52` | nRF52 (nRF52840 tested) | CTRL-AP unlock, NVMC, UICR save/restore around ERASEALL |
| `nrf53` | nRF5340, core 0 application, core 1 network | see the APPROTECT trap below |
| `stm32u5` | STM32U575/585 (DEV_ID 0x482), U59x/5Ax (0x481) | TrustZone off only; internal flash through a streaming RAM loader; RDP 1 → 0 unlock |
| `generic` | any Cortex-M (registered last; `attach(..., raw=True)` forces it) | memory access, halt, reset; no flash |

Measured on r5 (2026-11-10, dgpio 8 MHz, `host/python/examples/flash_image.py` and `parallel_flash.py`):
| Target | Whole-flash image | Erase | Program | On-target CRC | Total |
|---|---|---|---|---|---|
| nRF5340 app core | 1 MB | 0.2 s (NVMC ERASEALL, UICR kept) | 3.08 s (333 KB/s) | 0.14 s | 3.2–3.5 s |
| nRF5340 network core | 256 KB | fast path (UICR kept) | | | 0.71 s |
| STM32U585 | 2 MB random | 0.35 s (bank erase) | 6.37 s (322 KB/s) | 0.29 s | 7.0 s |
| STM32U585 | real 1.2 MB image (.hex, 4 page runs) | page/bank erase | | | 4.2 s |

- **STM32U5 streaming loader** (`loaders/stm32u5/`, built and checked in an emulator by its `build.py`): the probe
  writes the next 4 KB buffer into SRAM1 while the target's CPU programs the previous one (128-byte bursts, blank
  quad-words skipped). Before it starts, the module reset-halts the part once and raises the core clock from
  4 MHz to MSIS 48 MHz (voltage range 1, 4 wait states), mostly for the loader's CRC-32 (2 MB in 0.29 s instead
  of 3.9 s). Against the probe-driven bursts (154 KB/s, kept as the `ESW_U5_DIRECT` build option) programming went
  2.1x faster: the flash was never the limit, SWD writes paced by WAIT polling were. Now the probe's SWD/USB link
  is (about 330 KB/s per channel), and the probe's CPU no longer polls WAIT (2.6 M WAITs per 2 MB before, 94 now).
- `Esw.flash()` leaves out 0xFF stretches of 256 bytes or more after the erase (`skip_blank`); the CRC still covers
  the whole range.
- **nRF5340 APPROTECT trap.** Debug access needs both the UICR branch and a firmware branch that the image's startup
  code (Nordic's `SystemInit` → `nrf53_handle_approtect()`) reopens after every reset. Flash only images that run it:
  any other image (or a half-written one) shuts the port at the next pin reset, and only ERASEALL brings it back.
  ERASEALL also wipes the UICR's KMU key slots, which a debugger cannot read (0xDEADDEAD) or save. The module's fast
  whole-flash erase runs only after proving every KMU slot empty; otherwise it erases page by page (87 ms each).
- Flash work starts with one reset-halt per session (nRF5340, STM32U5): a running firmware's protections are gone
  (MCUboot/TF-M had locked the nRF5340's first 16 KB in the SPU: FLASHREGION[0].PERM = 0x115).

## Parallel flashing
`parallel_flash.py --a IMAGE --b IMAGE` flashes both channels one after the other, then from two host threads at
once. nRF5340 app 1 MB + STM32U585 1.2 MB (.hex):
| Firmware | Sequential | Parallel (wall) | Speed-up (ideal) |
|---|---|---|---|
| 0.3 (one task), STM32 probe-driven | 14.1 s | 14.6 s | 0.96× |
| 0.4, STM32 probe-driven, on channel B (core 0) | 14.9 s | 12.5 s | 1.19× (1.31×) |
| 0.4, STM32 probe-driven, on core 1 | 14.6 s | 10.8 s | 1.34× (1.43×) |
| 0.4, STM32 streaming loader, blank stretches skipped | 7.45 s | **5.5 s** | 1.35× (1.75×) |

Both channels are now limited by the USB link, not by SWD or the flash: USB-Serial-JTAG carries at most about
485 KB/s toward the board (2 KB frames, no SWD), and the two channels together reach about 390 KB/s. A faster
link (the TinyUSB vendor interface) is the next step for parallel flashing.

## CRC verify loader
`TGT_CRC32` computes `zlib.crc32` of target memory on the target, using the built-in loader
(`lib/core/loader/crc32_blob.inc`, generated).
- Rebuild it with `../.venv/bin/python ../loaders/crc32/build.py`, which needs the Arm GNU toolchain in `~/arm`.
- The generator re-verifies the blob in unicorn before it writes the `.inc`.

## DUT UART bridge
Each channel's DUT UART is bridged over the same framed protocol (`lib/include/esw/proto.h`, Python `Esw.uart_*`):

| Command | What it does |
|---|---|
| `UART_OPEN` (0x30) | Configures the UART (baud 300 to 5 M, parity, stop bits) and holds the UART switch group. The switches close only once VTref is valid. Sent again, it changes the settings. Returns the actual baud rate and the back-end (DMA or IRQ) |
| `UART_CLOSE` (0x31) | Waits for TX to drain, stops the stream and releases the hold (the switches open) |
| `UART_WRITE` (0x32) | Queues TX data and returns how much fit. Refused with `ESW_ERR_INTERLOCK` unless the group is closed and the pads are routed |
| `UART_STATS` (0x33) | Counters: rx, tx, USB drops, FIFO overflows, buffer-full events, framing/parity/break errors |
| `CH_MODE` (0x13, protocol 5) | Switch mode per group: `auto` (closed while used), `on` (closed whenever VTref is valid), `off` (never closed). See `host/python/README.md` |
| event 0xE0 | RX data, pushed unsolicited: channel, flags (data lost / line error / break), device timestamp (µs), up to 2 KB |

How it works:
- **UART1 is channel A, UART2 is channel B.** Each runs on one of two back-ends (`ports/esp32s3/esw_uart.c`). Both
  have a 32 KB RX ring (320 ms at 1 Mbaud) and an 8 KB TX ring.
  - **DMA:** UART → UHCI → GDMA into a circular chain of 32 × 256 B descriptors that never stops. A line-idle EOF
    only closes the current descriptor early. The GDMA interrupt (IRAM) moves finished descriptors into the ring.
    The hardware alone buffers 8 KB (80 ms at 1 Mbaud). TX is also DMA. IDF's `uhci` driver is not used: it stops
    and re-arms after every EOF and cannot be deleted while the line is quiet.
  - **IRQ:** the ESP-IDF driver, hardened: ISR in IRAM (`CONFIG_UART_ISR_IN_IRAM`), interrupt level 3, FIFO-full
    threshold 32 of 128 bytes (0.96 ms of margin at 1 Mbaud).
  - The ESP32-S3 has one UHCI. The DMA back-end goes to the first channel opened while it is free (open the faster
    channel first). `ESW_UART_NO_DMA` keeps a channel on interrupts.
- **The `esw_uart` task (core 0, every 1 ms):**
  - it starts and stops back-ends and routes the pads (proto task requests go through a queue);
  - it moves RX data into event frames;
  - it owns the UART and GDMA interrupts on core 0. Channel A's SWD engine (core 1) never delays them; channel B's
    engine shares core 0 and masks interrupts for one packet at a time (about 7 µs at 8 MHz), well inside the RX
    margin (DMA: 8 KB in hardware; IRQ back-end: 0.96 ms at 1 Mbaud). Not yet measured with both at full load.
- **Host stalls:** an event that USB cannot take yet stays queued and is retried, while the UART ring absorbs the
  backlog. Data is dropped only after USB has refused it for 1 s.
  - Do not use `usb_serial_jtag_is_connected()` for decisions like this. Its start-of-frame monitor allows 3 missed
    ticks in total, so under load it reports brief false disconnects. It cost 120 bytes before this was changed.
- **Pin routing:** the board's prepare step runs before every closing of the switch group. It puts TX on a plain
  GPIO driven high. `esw_uart_route()` then switches the GPIO-matrix source back to the UART, keeping the TX output
  enabled, so the DUT never sees an edge. Routing happens once per closing, latched by the channel's `opened[]`
  counter. (`uart_set_pin()` would release TX first and float the DUT's RX line.)
- **Host loss:** the channel releases the hold, and `supervise()` closes the stream.
- **Keep-alive:** the Python client PINGs every second while idle, so a listen-only sniffer is not taken for a lost
  host.

**Signal integrity (measured on r5, jumper wires to an nRF52840 DK, full duplex):**
- With full TX drive strength, our TX edges coupled into our own RX, and the ESP32-S3 receiver sampled the glitches
  as bits. DK → tool errors per byte: 9e-5 at 115200, 7e-4 at 460800. Tool → DK stayed clean: the nRF receiver is
  immune.
- Defaults are therefore the weakest TX drive (`GPIO_DRIVE_CAP_0`) and the RX glitch filter at 1/8 bit.
  - Each alone fixes it. Both together: **0 errors in 600 KB per direction at 1 Mbaud**, and none at 115200 or
    460800.
  - Flags `ESW_UART_TX_STRONG` and `ESW_UART_RX_NOFILT` switch them back for diagnosis.

**Self-test without a DUT (protocol 5):** `ESW_UART_LOOPBACK` in `UART_OPEN` loops TX to RX inside the ESP32-S3
UART (`UART_CONF0.loopback`). The pads stay on their safe idle state and the switch group is not held, so it needs
no wiring and touches no DUT. `host/python/examples/uart_loopback.py` streams both channels at once through it:
- 1 Mbaud: 97.6 KB/s each way per channel for 10 s, 0 lost (DMA back-end on A, IRQ back-end on B);
- 2 Mbaud: 195 KB/s each way per channel, about 780 KB/s over USB-Serial-JTAG in total, 0 lost, 0 FIFO overflow.

Through the host daemon's PTYs (`esw serve --test-loopback`, `examples/tty_loopback.py`): the same 97 KB/s each
way at 1 Mbaud on both TTYs, byte-exact, also while the STM32 flashed and the nRF5340 was read back.

**Real pads, A ↔ B (2026-11-11):** J302 and J402 cross-cabled, through both channels' switches, via the TTYs
(`tty_loopback.py --cross`): both directions at once byte-exact at 115200, 460800, 1 M and 2 Mbaud, 8E1 included;
20 s at 1 Mbaud during a dual-DUT flash with 0 line, overflow or buffer errors on the board.

**Stress test:** `host/python/examples/uart_stress.py`. It runs everything at once:
- channel A in full duplex with the DK;
- channel B through a loopback jumper;
- SWD reads;
- a flash sector erase every 250 ms (test hook `0x7D`), which stops the flash cache for about 40 ms each time.

Measured on channel A alone, 30 s at 1 Mbaud: 2.5 MB each way, 4,000 SWD reads, 104 erases. **0 errors and 0
overflows on both back-ends.**

Back-to-back erases stop the tasks and USB altogether. Even then the DMA back-end recorded 0 FIFO overflows; the
software ring filled only because nothing could reach the host. The firmware itself never writes flash while
streaming.

**Hardware test:** `host/python/examples/uart_test.py`, with the wiring in its docstring. J302 GND/TX/RX go to DK
GND / P1.01 / P1.02. The test firmware is `testfw/nrf52840_uart/`, and the script flashes it over SWD. It checks:
- the boot banner on an SWD reset and on a pin reset;
- echo at 115200, 460800 and 1 M;
- full duplex at 1 M;
- a 1 MB stream with SWD reads in parallel, through a 250 ms host stall;
- 256 KB to the DK with a CRC computed on the DK;
- isolation (no RX, TX refused) and re-routing after the switch closes again;
- error counters on both ends.

## Crash safety
A panic resets only the CPUs, so the GPIO output latches survive. The isolation enables are therefore driven low in
three places:
1. `__wrap_esp_panic_handler` (link option `--wrap=esp_panic_handler`): the first action of the panic handler.
2. A shutdown handler: every `esp_restart()`.
3. `bootloader_components/esw_boot_hooks`: `bootloader_before_init`, the first function in the 2nd-stage bootloader.
   After adding or renaming a bootloader component, delete `build/bootloader` once: the bootloader sub-build scans
   `bootloader_components/` only when it is configured.

Check it on hardware with `host/python/examples/robustness_check.py`. It needs this test build: `ESW_TEST_HOOKS`
enables the `TEST_PANIC` command.

## Updating later (no buttons on r5)
1. **`esw update app/build`** (or `esw update merged.bin` from `idf.py merge-bin`), **`idf.py flash`**, or plain
   esptool over USB-Serial-JTAG. This works while this firmware runs and even if it hangs: the reset into the
   downloader is done by the USB-Serial-JTAG peripheral. `esw update` suspends a running `esw serve` around the
   write. Keep it that way:
   - never reconfigure GPIO19/20;
   - never switch the USB PHY to OTG without the measures below.
2. **`REBOOT` command, mode 1** (`Esw.reboot(rom_download=True)`): sets the RTC force-download flag and restarts.
3. **OTA (planned).** The partition table already has two app slots, otadata and rollback
   (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). The app confirms itself 500 ms after start. An over-the-protocol update
   command will write the inactive slot.

When the firmware moves to TinyUSB (USB-OTG: vendor interface, CDC, CMSIS-DAP), path 1 no longer works while it
runs. Paths 2 and 3 then become mandatory, and so does a watchdog-safe return to USB-Serial-JTAG.

## One-time crossing from MicroPython (2026-11-06, board 2)
MicroPython 1.29 switches the USB PHY to OTG (`303a:4001`), so esptool's hardware reset cannot reach the chip.
What failed:
- `machine.bootloader()`: the USB hung;
- the 1200-baud touch: not implemented;
- `esp.flash_erase(0)`: blocked by IDF's dangerous-write protection, the board panicked and rebooted.

What worked:
1. From the MicroPython REPL, write the RTC registers. They live in the RTC domain, so they survive a system reset.
   ```
   import machine
   machine.mem32[0x6000812C] |= 1                              # RTC_CNTL_OPTION1_REG: FORCE_DOWNLOAD_BOOT
   machine.mem32[0x60008120] &= 0xFFE7FFFF                     # RTC_CNTL_USB_CONF_REG: PHY back to USB-Serial-JTAG
   machine.mem32[0x60008000] |= 0x80000000                     # RTC_CNTL_OPTIONS0_REG: SW_SYS_RST
   ```
2. The PC does not notice: the pull-up never drops, and the old device answers `Errno 71`. Run
   `sudo usbreset 303a:4001`. It reports "No such device" because the device vanishes and comes back as `303a:1001`
   (the ROM downloader).
3. Run `idf.py flash`.

Do **not** replug between steps 1 and 3: a power cycle clears the RTC registers and MicroPython boots again.

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

The dgpio engines are created on core 1 at boot (`esw_proto` task) and must only be used from there. Every attach
re-routes SWCLK/SWDIO to the dedicated signals and self-checks: the board's pin setup runs `gpio_config`, which
re-routes the pads to plain GPIO.

- Below about 480 kHz interrupts stay enabled during packets. A wait that ends more than 16 cycles late restarts the
  schedule from now, so an interrupt stretches one clock phase and never produces catch-up bursts.
- The host keeps at most 3 frames in flight (`Esw.MAX_WINDOW`): the board's USB receive buffer is 8 KB, with no flow
  control.

Benchmarks:
- `host/python/examples/swd_bench.py --phy 1 --clocks 1e6,4e6,8e6`: on the device, no USB in the loop.
- `flash_speed.py --size 1M --window 3 --phy 1 --verify crc`: full flow.

## CRC verify loader
`TGT_CRC32` computes `zlib.crc32` of target memory on the target, using the built-in loader
(`lib/core/loader/crc32_blob.inc`, generated).
- Rebuild it with `../.venv/bin/python ../loaders/crc32/build.py`, which needs the Arm GNU toolchain in `~/arm`.
- The generator re-verifies the blob in unicorn before it writes the `.inc`.

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
1. **`idf.py flash` / esptool over USB-Serial-JTAG.** This works while this firmware runs and even if it hangs: the
   reset into the downloader is done by the USB-Serial-JTAG peripheral. Keep it that way:
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

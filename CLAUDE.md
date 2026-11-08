# CLAUDE.md

Firmware for the ESP32-S3 dual SWD flasher, board **r5**. Read `docs/hardware-reference.md` before writing code
that touches a GPIO. It is the source of truth for the hardware. `docs/hardware/design-brief.md` holds the product
requirements.

## Hardware is fixed
- The boards are r5 and cannot change. Treat `hardware/rev5/` as read-only reference.
- Hardware changes for later revisions are made in the KiCad project at
  `/home/adrian/development/pcb_ai/dual_swd_flasher`, in a separate session. If firmware needs a hardware change,
  write it down (e.g. `docs/hw-change-requests.md`) instead of editing KiCad files here.
- Never edit `.kicad_*` files with text tools.

## Rules the firmware must keep
- **Isolation enables** (A: GPIO9/46/10, B: GPIO40/41/42) are active high with external 100 kΩ pull-downs.
  - Configure each as an output driven **low** before anything else touches it.
  - Never enable internal pull-ups on them.
  - On panic, reset or deep sleep the switches must end up open. The pull-downs do this when the pins are released;
    do not hold the pins high.
- **Connect interlock:**
  1. Close EN_VTREF.
  2. Wait ≥ 25 ms.
  3. Close EN_SWD or EN_UART only if VTref ≥ about 2.8 V (ADC pin ≥ about 1.4 V).
  4. Open everything if VTref drops.
  - Closing the signal groups to an unpowered DUT back-powers it.
- **nRESET:** GPIO7 (A) and GPIO21 (B) driven **high** pull the DUT reset low, through a 2N7002. Default is low
  (released). It reaches the DUT only while EN_SWD is closed. Read back on GPIO15 (A) and GPIO47 (B).
- **VTref divider:** with EN_VTREF closed, the DUT supply carries about 16.5 µA. Open EN_VTREF in measurement mode.
- **Reflashing:** there are no BOOT or RESET buttons.
  - A firmware that takes over USB-OTG (TinyUSB, CDC-NCM) must keep a way back to the ROM downloader: a command that
    gets the chip back to an esptool-reachable ROM downloader. Setting the RTC force-download flag and restarting is
    **not enough** while USB-OTG owns the PHY: the PHY mux is in the RTC domain and survives the restart. Either:
    - (a) set the ROM USB-OTG persist flags first, as IDF's `esp_usb_console_before_restart()` does; the ROM then
      enumerates as OTG CDC `303a:0002`; or
    - (b) detach from the bus first, then map the PHY back to USB-Serial-JTAG (`RTC_CNTL_USB_CONF_REG` bits 19/20 = 0),
      set the flag and reset. Without a detach the host does not re-enumerate: seen on 2026-11-06, it needed
      `usbreset`.

    Bench-test this path with the GPIO0 short at hand before any OTG image is deployed.
  - Last resort: short module pad 27 (GPIO0) to GND while plugging in USB.
- **Flash size:** the fitted modules are ESP32-S3-WROOM-1-N4 (4 MB flash, no PSRAM). Never assume PSRAM.
  - Do not use octal-PSRAM sdkconfig options: GPIO35 is B_UART_RX.

## Environment (as of 2026-11-06)
- **Board:** `/dev/ttyACM0`. USB `303a:1001` means ROM or USB-Serial-JTAG; `303a:4001` means MicroPython.
- **Board currently on this PC:** board 2 (MAC …ef:34).
  - It runs **esp-swd firmware** (`app/`, ESP-IDF v6.0.3) on USB-Serial-JTAG: `303a:1001`, `/dev/ttyACM0`.
  - Flash with `idf.py -p /dev/ttyACM0 flash`; no button needed. See `app/README.md`.
  - MicroPython is gone from this board. The crossing procedure is in `app/README.md`.
- **ESP-IDF:** `~/esp/esp-idf-v6.0.3` (tools in `~/.espressif`, 4.7 GB).
  - Use `. ~/esp/esp-idf-v6.0.3/export.sh`, then `export PATH="$PATH:<repo>/.venv/bin"` for cmake/ninja.
  - Append to PATH, never prepend: prepending shadows IDF's Python.
- **Python:** system Python 3.14 now has venv/ensurepip and pyserial 3.5.
  - Repo venv `.venv/` (gitignored) holds **mpremote 1.29.0**, plus capstone and unicorn for checking hand-made target
    images: `python3 -m venv .venv && .venv/bin/pip install mpremote capstone unicorn`.
- **mpremote gotcha:** never soft-reset the board. No Ctrl-D, no `soft-reset`, and no plain `mpremote exec`, which
  soft-resets on connect. MicroPython 1.29's soft reset hung the native USB twice on 2026-11-06; only a replug
  recovered it. Use `mpremote connect <port> resume ...`, and `machine.reset()` for a fresh interpreter
  (`firmware/poc_swd/run.sh` does this).
- **Board test:** `firmware/poc_swd/` (MicroPython): staged SWD diagnostics against an nRF52840 DK on channel A. It
  is a throwaway hardware check; production firmware will be ESP-IDF/FreeRTOS with DMA-driven SWD.
  - 2026-11-06: full pass on board 2 channel A (connect, unlock, flash, run).
  - The DK now holds the 64-byte LED-chase test image.
- **Arm GNU toolchain:** 15.3.rel2 in `~/arm/arm-gnu-toolchain-15.3.rel2-x86_64-arm-none-eabi` (1.1 GB). It is needed
  only to rebuild target-side loaders (`loaders/crc32/build.py`); the library includes the generated `.inc`.
- **SWD engine rule:** the ESP32-S3 dedicated-GPIO engine (`ports/esp32s3/esw_dgpio.c`) is per CPU core.
  - Create it on the core that uses it (core 1, `esw_proto`) and never delete it.
  - Call `esw_dgpio_attach()` after every isolation-group close: `gpio_config` on its pins silently re-routes them.
- **Library build and tests (host):** `PATH=$PWD/.venv/bin:$PATH cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
  - Also run with `-DESW_SANITIZE=ON` (ASan/UBSan).
  - `lib/` must stay portable: C11, no RTOS, no vendor headers, no malloc, `-Wconversion -Werror` clean.
- **Git:** commit only when the user asks. Remote: `https://github.com/adihack/esp32-swd` (public, MIT).
  - Never commit vendor datasheets or vendor 3D models (gitignored); new source files get an
    `SPDX-License-Identifier: MIT` header.

# CLAUDE.md

Firmware and host tools for the ESP32-S3 dual SWD flasher, board **r5** (project name esp-swd).

**Start of a session:** read `docs/status.md` (where the project stands, decisions, what is next) and
`docs/testing.md` (every hardware test, its wiring and expected result). Read `docs/hardware-reference.md` before
writing code that touches a GPIO: it is the source of truth for the hardware. `docs/hardware/design-brief.md` holds
the product requirements; `host/python/README.md` is the user guide for the `esw` tools.

## Hardware is fixed
- The boards are r5 and cannot change. Treat `hardware/rev5/` as read-only reference.
- Hardware changes for later revisions are made in the KiCad project (on the original PC:
  `/home/adrian/development/pcb_ai/dual_swd_flasher`), in a separate session. If firmware needs a hardware change,
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
- **DUT firmware is the user's:** ask before writing a DUT's flash, UICR or option bytes.
- **nRF5340:** flash only images that run Nordic's startup code. Its debug port has a firmware APPROTECT branch that
  closes at every reset until `SystemInit` reopens it; a random or half-written image locks the debugger out at the
  next pin reset (detach pulses nRESET). Recovery is CTRL-AP ERASEALL (`unlock` with allow-erase), which also wipes
  the UICR's KMU key slots (unreadable to a debugger).
- **VTref about 2.7 V from a 3.3 V DUT** means the 10-pin cable is rotated (pin 1 on the DUT's nRESET pull-up).
  Never lower the 2.8 V threshold to "fix" it.
- **Channels run in parallel**, one worker per CPU core (A on core 1, B on core 0). Anything that issues SWD packets
  for a channel must run in that channel's worker.
- **Switch modes** (protocol 5): `auto` / `on` (host pin holder) / `off` (inhibit). No mode bypasses the VTref
  interlock; keep it that way.
- **USB stays on USB-Serial-JTAG for now** (user decision 2026-11-11, MVP): it keeps esptool's hardware reset into
  the ROM downloader, so `esw update` recovers even a hung firmware. TTYs come from the host daemon (PTYs), not
  from USB CDC.
- **Flash size:** the fitted modules are ESP32-S3-WROOM-1-N4 (4 MB flash, no PSRAM). Never assume PSRAM.
  - Do not use octal-PSRAM sdkconfig options: GPIO35 is B_UART_RX.

## Machine setup
New machine: follow "New machine" in `README.md` (ESP-IDF v6.0.3, repo venv, `pip install -e host/python`, udev
rule, optional Arm toolchain). Gotchas:
- After `. ~/esp/esp-idf-v6.0.3/export.sh`, **append** the repo venv to PATH (`export PATH="$PATH:<repo>/.venv/bin"`)
  for cmake/ninja; prepending shadows IDF's Python.
- The Arm GNU toolchain (15.3.rel2, `~/arm/arm-gnu-toolchain-*` or `ARM_GCC_BIN`) is needed only to rebuild
  target-side loaders (`loaders/*/build.py`) and DUT test firmware (`testfw/`). The library includes the generated
  `.inc` files. The loader build scripts verify each blob in unicorn (`pip install unicorn capstone`).
- Board on USB: `303a:1001` (USB-Serial-JTAG, also the ROM downloader). `303a:4001` would be MicroPython (only
  seen during bring-up; the crossing procedure is in `app/README.md`). `firmware/poc_swd/` and
  `firmware/bringup/` are MicroPython history, kept for reference.

## Working rules and gotchas
- **Host tools:** `.venv/bin/pip install -e host/python` gives `.venv/bin/esw` (CLI); `esw serve` is the daemon
  (TTYs + socket at `$XDG_RUNTIME_DIR/esp-swd/<board>/`). While it runs, it holds the port exclusively and
  `Esw()` goes through it; `examples/robustness_check.py` needs it stopped.
  - Stop it with `esw stop`. Never `pkill -f`/`pgrep -f` with a pattern that also appears in the same shell
    command: it matches its own shell and kills the command.
  - `esw serve --test-loopback` + `examples/tty_loopback.py`, and `examples/uart_loopback.py`, test both UARTs at
    1 Mbaud with no DUT and no wiring (internal loopback).
  - Python 3.14 maps `termios.Bxxx` to plain numbers: read PTY speeds from `struct termios2.c_ospeed` (TCGETS2),
    not from the Bxxx bits. Linux PTYs drop PARENB, so parity is a daemon setting (`--format-a 8E1`).
  - USB-Serial-JTAG does not re-enumerate on `esp_restart()`: a host sees no disconnect after REBOOT or a panic.
    The daemon detects restarts by the INFO boot id.
  - The board's USB RX ring (8 KB) drops data when full: every host path keeps the in-flight budgets
    (`Esw._budget`, `server.Daemon._pump`).
- **UART:** TX stays at the weakest drive strength with the RX glitch filter on (crosstalk into our RX, measured).
  Never use `usb_serial_jtag_is_connected()` to decide anything quickly: it reports false disconnects under load.
- **SWD engine:** the ESP32-S3 dedicated-GPIO engine (`ports/esp32s3/esw_dgpio.c`) is per CPU core. Each channel's
  worker creates its own on its core and never deletes it. Call `esw_dgpio_attach()` after every isolation-group
  close: `gpio_config` on its pins silently re-routes them.
- **Library build and tests (host):** `PATH=$PWD/.venv/bin:$PATH cmake -S . -B build -G Ninja && cmake --build build
  && ctest --test-dir build`; also with `-DESW_SANITIZE=ON` (ASan/UBSan) in `build-asan`. Python:
  `.venv/bin/python -m unittest discover -s host/python/tests`.
  - `lib/` must stay portable: C11, no RTOS, no vendor headers, no malloc, `-Wconversion -Werror` clean.
- **Firmware and host change together:** a protocol change bumps `ESW_PROTO_VERSION` (`lib/include/esw/proto.h`)
  and `PROTO_VERSION` (`host/python/esw/proto.py`); bump `FW_NAME` (`app/main/main.c`) and the host package version
  (`host/python/pyproject.toml`) with it.
- **Git:** commit only when the user asks. Remote: `https://github.com/adihack/esp32-swd` (public, MIT).
  - Never commit vendor datasheets, vendor 3D models (gitignored) or DUT firmware images; new source files get an
    `SPDX-License-Identifier: MIT` header.

## Bench as of 2026-11-12 (original PC)
- **Board 2** (MAC f0:f5:bd:57:ef:34, board id `F0F5BD57EF34`) runs esp-swd **0.5.0** (protocol 5). Board 1
  (…ef:48) had its USB D− re-soldered and holds no esp-swd firmware yet.
- **Channel A (J301):** the user's nRF5340 product DUT. **Channel B (J401):** a separate STM32U585 board (RDP 0,
  TrustZone off). The user declared both test-only (2026-11-11). Both run Zephyr.
- **UART:** J302 and J402 are wired to the two DUTs' consoles (Zephyr, 115200 8N1): J30x.2 (tool TX) → DUT RX,
  J30x.3 (tool RX) ← DUT TX, GND common. Check: `esw serve`, open ttyA/ttyB at 115200, `esw reset A` (or `B`) →
  boot log. TX/RX swapped shows as 0 bytes **and** 0 framing errors (the RX line never moves).
  - For throughput tests without DUT traffic the headers can be cross-cabled instead (J302.2 → J402.3,
    J402.2 → J302.3; never join the two pin 4s: they are the two DUT supplies): `examples/tty_loopback.py --cross`.
- **nRF52840 DK:** not connected. The DK-based tests (`uart_test.py`, `uart_stress.py`, `robustness_check.py`,
  `nrf52_blinky.py`, `flash_speed.py`) have not run on firmware 0.5.0; see `docs/testing.md`.

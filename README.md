# esp-swd

**esp-swd** is an ESP32-S3 based dual-target SWD flasher and UART bridge with DUT isolation: open hardware, firmware,
a portable C library and a Python host library. It is built for CI and production benches, where a probe stays
plugged in for weeks and flashes the same known 3.3 V targets over and over.

The r5 board sits between a host PC (USB-C) and a device under test (DUT) that carries two MCUs, one per channel.
It does three things:
- programs each MCU over its own SWD port;
- bridges each DUT UART to the host;
- on command, opens analog switches on every DUT line, so the DUT's sleep current can be measured without leakage
  through the tool.

![r5 board](docs/images/board_3d_iso.png)

## Status (2026-11-09)
Work in progress. What exists and is tested on hardware (board r5, channel A, nRF52840 DK):
- **Isolation interlock:** VTref is checked before any signal switch closes. The switches reopen on VTref loss, host
  loss, reboot and firmware panic.
- **SWD engines:** an ESP32-S3 dedicated-GPIO engine up to 8 MHz (10 MHz cap), and a portable bit-bang fallback.
  Soak: about 3 M packets each at 8 MHz and 10 MHz, 0 errors.
- **Flashing:** 1 MB of random data on an nRF52840 takes **3.42 s** in total: mass erase 0.17 s, program 3.08 s,
  on-target CRC-32 verify 0.17 s. The data path is PC → USB → ESP32-S3 → SWD → NVMC.
- **UART bridge (channel A):** byte-exact at 115200, 460800 and 1 Mbaud, full duplex.
  - A 1 MB stream arrived intact while SWD reads ran in parallel and the host stopped reading for 250 ms.
  - With the switches open nothing gets through, and writes are refused.
- **Firmware update:** `idf.py flash` over USB while the firmware runs; two OTA slots with rollback.
- **Target modules (2026-11-10):** nRF52, **nRF5340** (application and network core) and **STM32U575/585**, plus a
  generic Cortex-M fallback. Measured: nRF5340 1 MB app image erase + program + CRC verify in 3.5 s; STM32U585 burst
  programming at 154 KB/s (flash-bound), RDP level 1 → 0 unlock tested.
- **Parallel channels:** one SWD worker per channel on its own CPU core. nRF5340 + STM32U585 flashed at once:
  10.8 s against 14.6 s one after the other.

| Not done yet |
|---|
| SWO and channel-B UART (untested on hardware); UART PTY bridge for terminal tools |
| TinyUSB vendor interface with USB TTYs, optional CMSIS-DAP v2 |
| STM32U5 with TrustZone on, external-flash loaders on the streaming ABI, nRF5340 network-core fast path test |
| multi-day soak |

Hardware is frozen at r5 (design revision d, JLCPCB, 2026-10-23). The two assembled boards carry
ESP32-S3-WROOM-1-**N4** modules (4 MB flash, no PSRAM), not the N16 in the BOM.

## Repository layout
| Path | Content |
|---|---|
| `lib/` | **Portable C11 library**: SWD/ADIv5, Cortex-M control, generic RAM-loader runtime (call and streaming ABIs), on-target CRC verify, target-module interface and the nRF52 module, isolation interlock, framed host protocol. No RTOS, no malloc. See `lib/README.md` |
| `ports/esp32s3/` | ESP32-S3 port: dedicated-GPIO SWD engine, pins, ADC, clock |
| `boards/r5/` | r5 board layer: pin map, switch groups, VTref divider, safe defaults |
| `app/` | ESP-IDF v6 firmware for r5 (USB-Serial-JTAG transport). See `app/README.md` |
| `host/python/` | Python host library (`esw`) and examples: flashing, speed test, PHY benchmark, robustness checks |
| `host/udev/` | udev rule for the board |
| `loaders/` | Target-side loader sources; `crc32/build.py` rebuilds and verifies the built-in CRC loader |
| `testfw/` | Bare-metal test firmware for DUTs, e.g. `nrf52840_uart/` for the UART hardware test |
| `tests/` | Host unit tests against a bit-level SWD target simulator (`tests/sim/`); CI in `.github/workflows/ci.yml` |
| `docs/hardware-reference.md` | **Start here for hardware.** Pin map, signal chains, polarities, connectors, test pads, interlock rules, boot notes, bring-up results |
| `docs/firmware-architecture.md` | Firmware architecture, reliability plan, milestones, decisions |
| `docs/hardware/` | Design brief, hardware architecture history, PCB layout notes |
| `docs/datasheets/` | Links to the vendor datasheets (not redistributed) |
| `hardware/rev5/` | r5 schematic PDF, KiCad 10 project as built, the exact JLCPCB production package |
| `firmware/bringup/`, `firmware/poc_swd/` | MicroPython bring-up blink and the first SWD board test (kept for reference) |

## Quick start
Library and tests on a PC:
```
python3 -m venv .venv && .venv/bin/pip install cmake ninja pyserial
PATH=$PWD/.venv/bin:$PATH
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

Firmware (ESP-IDF v6.0.3); no buttons are needed, USB-Serial-JTAG resets the chip into the ROM downloader:
```
. ~/esp/esp-idf-v6.0.3/export.sh
export PATH="$PATH:$PWD/.venv/bin"
cd app && idf.py build && idf.py -p /dev/ttyACM0 flash
```

Host examples (the board enumerates as `303a:1001`, `/dev/ttyACM0`):
```
.venv/bin/python host/python/examples/nrf52_blinky.py --channel A          # attach, flash a test image, watch it run
.venv/bin/python host/python/examples/flash_speed.py --size 1M --window 3 --verify crc
.venv/bin/python host/python/examples/uart_test.py                          # UART: wiring in its docstring
.venv/bin/python host/python/examples/flash_image.py --info --channel B      # identify a target, print its memory map
.venv/bin/python host/python/examples/flash_image.py --channel B build/app.hex   # .hex or .bin (--addr), CRC-verified
.venv/bin/python host/python/examples/parallel_flash.py --a app_a.hex --b app_b.hex   # both channels at once
```
nRF5340: `--core 1` selects the network core. Flash only images with Nordic's startup code (see `app/README.md`,
"nRF5340 APPROTECT trap").
`nrf52_blinky.py --allow-erase` mass-erases a locked (APPROTECT) chip. Without it, a locked chip is left alone.

## Hardware in one screen
| Function | Channel A | Channel B |
|---|---|---|
| SWDIO / SWCLK | GPIO4 / GPIO5 | GPIO11 / GPIO12 |
| SWO (in) | GPIO6 | GPIO14 |
| nRESET drive (high = assert) / sense | GPIO7 / GPIO15 | GPIO21 / GPIO47 |
| UART TX / RX | GPIO16 / GPIO17 | GPIO48 / GPIO35 |
| VTref ADC (reads VTref/2) | GPIO8 (ADC1_CH7) | GPIO1 (ADC1_CH0) |
| EN_SWD / EN_UART / EN_VTREF (active high) | GPIO9 / GPIO46 / GPIO10 | GPIO40 / GPIO41 / GPIO42 |

- Status LED: GPIO2, active high.
- USB: native, on GPIO19/20.
- No buttons.
- DUT I/O is 3.3 V only; there are no level translators.
- Every switch is **open by default** (100 kΩ pull-downs on the enables).
- Firmware must close EN_VTREF and see the DUT powered **before** closing the SWD or UART groups. Otherwise the tool
  back-powers an unpowered DUT.

All details, with the reasons, are in `docs/hardware-reference.md`.

## License
MIT, see `LICENSE`. This covers the firmware, library, host tools, documentation and the r5 hardware design files.
Vendor datasheets and the USB-C connector's 3D model are not part of the repository; see `docs/datasheets/README.md`.

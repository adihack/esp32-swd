# Testing esp-swd

How to check the library, the host tools and the firmware: software tests that run anywhere, then hardware tests
grouped by bench setup. Each test lists its wiring, command and the result measured last, so a run can be compared
at a glance. Setup of a new machine: `README.md`, "New machine". Tool usage: `host/python/README.md`.

All commands run from the repository root unless noted. `esw` is `.venv/bin/esw` (after
`.venv/bin/pip install -e host/python`); examples are `.venv/bin/python host/python/examples/<name>.py`.

## 1. Software tests (no hardware; CI runs them on every push)
| What | Command | Expected |
|---|---|---|
| C library against the bit-level SWD target simulator | `PATH=$PWD/.venv/bin:$PATH cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build` | 6/6 suites pass |
| Same with AddressSanitizer + UBSan | the same with `-B build-asan -DESW_SANITIZE=ON` | 6/6 |
| Host daemon against a simulated board on a PTY; image handling | `.venv/bin/python -m unittest discover -s host/python/tests -v` | 16 tests OK (Linux) |
| Target-side loaders, rebuilt and run in the unicorn emulator | `.venv/bin/python loaders/crc32/build.py`, `.venv/bin/python loaders/stm32u5/build.py` | "verified" lines; needs the Arm toolchain and `pip install unicorn capstone` |
| Firmware builds | `cd app && idf.py build` (ESP-IDF v6.0.3 exported, venv appended to PATH) | `Project build complete` |

## 2. Firmware on the board
```
esw update app/build              # or: cd app && idf.py -p /dev/ttyACM0 flash
esw info                          # firmware: esp-swd 0.5.0 r5/usj (protocol 5)
```
- The firmware is always built with `ESW_TEST_HOOKS` (`app/main/CMakeLists.txt`). That adds `TEST_PANIC` (0x7E) and
  the flash-erase stress (0x7D), which `robustness_check.py` and `uart_stress.py` need.
- Firmware and host tools must have the same protocol version: update both from the same commit.

## 3. Board only (no DUT, no wiring)
| Test | Command | Last result (2026-11-11, board 2, 0.5.0) |
|---|---|---|
| Both UARTs at once, internal loopback, through USB | `.venv/bin/python host/python/examples/uart_loopback.py --baud 1000000 --seconds 10` | 1 Mbaud: 97.6 KB/s each way per channel, 0 lost, 0 FIFO overflow; `--baud 2000000`: 195 KB/s, 0 lost |
| The same through the daemon's TTYs | `esw serve --test-loopback &` then `.venv/bin/python host/python/examples/tty_loopback.py` | 97 KB/s each way per channel, byte-exact (also 115200, 250000, 2 M) |
| Daemon survives reboots | with `esw serve` running: `esw reboot`, wait 2 s, run `tty_loopback.py` again (3×) | TTYs work 2 s after each reboot; the log shows "board restarted" each time |
| Firmware update through the daemon | `esw update app/build` with `esw serve` running | "running: esp-swd 0.5.0"; the daemon log shows suspend and reconnect |
| Merged image | `cd app && idf.py merge-bin`, then `esw update app/build/merged-binary.bin` | as above; an app-only `.bin` is refused with a hint |

`uart_loopback.py` needs the channels' UARTs free: stop the daemon, or `esw tty AB off` while it runs (`esw tty AB
open` afterwards).

## 4. Bench of 2026-11-11: nRF5340 on A, STM32U585 on B, UART headers cross-cabled
Wiring:
- J301 → nRF5340 DUT (10-pin Cortex debug cable); J401 → STM32U585 board. Each DUT powers its own VTref.
- J302.2 (A TX) → J402.3 (B RX) and J402.2 (B TX) → J302.3 (A RX). GND is common on the r5 board. **Never join the
  two pin 4s:** they are the two DUT supplies.
- Check first with `esw info`: after any SWD or UART use, VTref should read about 3.27 V on both channels. About
  2.7 V means a rotated 10-pin cable (pin 1 on the DUT's nRESET pull-up); the interlock then refuses, correctly.

| Test | Command | Last result |
|---|---|---|
| Identify both targets | `.venv/bin/python host/python/examples/flash_image.py --info --channel A` (and `B`) | `nRF5340-QKAA app`; `STM32U5 482 2048K` with its memory map |
| Real UART pads, both directions at once | `esw serve &`, then `.venv/bin/python host/python/examples/tty_loopback.py --cross --baud 1000000` | byte-exact at 115200, 460800, 1 M and 2 Mbaud; 97 KB/s each way at 1 Mbaud |
| Parity through the daemon | `esw serve --format-a 8E1 --format-b 8E1 &`, then `tty_loopback.py --cross` | byte-exact, 83 KB/s each way (11 bits per byte) |
| Shell tools | `stty -F <ttyA> 115200 raw -echo`; `cat <ttyB> &`; `echo hello > <ttyA>` | `hello` arrives on B |
| Switch modes on real pads | with ttyA/ttyB open: `esw mode B uart off` → nothing passes either way, B's writes are counted as dropped in `esw info`; `esw mode B uart auto` → data flows again | as described |
| Both DUTs at once | `esw flash A=<nrf5340 app image> B=<stm32 image>` | 5.66 s, both CRC-verified (one after the other: 3.7 s + 4.9 s) |
| Benchmark, sequential vs parallel | `.venv/bin/python host/python/examples/parallel_flash.py --a <nrf5340 image> --b <stm32 image>` | sequential 7.45 s, parallel 5.52 s (same with and without the daemon) |
| Everything at once | `tty_loopback.py --cross --seconds 20` in the background, `esw flash A=... B=...` meanwhile | 2 MB each way, 0 line/overflow errors (`Esw().uart_stats(ch)`); the flash verified in 11.5 s (the UARTs keep their rate) |
| Ownership | a script holding channel B (`Esw().connect(1)` and sleeping), then `esw flash B=...` | refused: "channel B SWD is in use by another program"; after `kill -9` of the script the daemon detaches and releases B |

Images: the DUTs' own firmware from their backups (kept outside the repository). **nRF5340: only images that run
Nordic's startup code**; a random or half-written image shuts the debug port at the next reset (see `app/README.md`,
"nRF5340 APPROTECT trap").

## 5. nRF52840 DK on channel A (not yet re-run on firmware 0.5.0)
These scripts were written against the DK and passed on firmware 0.2–0.4 (results in `docs/hardware-reference.md`,
section 12). They are the next regression to run.

| Test | Wiring | Command | Expected (last pass) |
|---|---|---|---|
| Blinky: attach, flash, run | J301 → DK P18 "Debug in" | `nrf52_blinky.py --channel A [--allow-erase]` | LED chase visible in P0.OUT |
| Flash speed | as above | `flash_speed.py --size 1M --window 3 --verify crc` | 1 MB erase + program + CRC in about 3.4 s |
| SWD PHY benchmark | as above | `swd_bench.py --phy 1 --clocks 1e6,8e6` | 0 errors; about 7.4 µs per packet at 8 MHz |
| UART test | plus J302.1 → DK GND, J302.2 → DK P1.01, J302.3 → DK P1.02 (needs the Arm toolchain: it builds and flashes `testfw/nrf52840_uart`) | `uart_test.py` (`--no-dma` for the interrupt back-end) | 22/22 on the DMA back-end, 21/21 with `--no-dma` (2026-11-09) |
| UART stress | as the UART test, plus channel B: J402.2 ↔ J402.3, J402.1 → DK GND, J402.4 → DK VDD | `uart_stress.py --duration 30` | 0 errors, 0 overflows on both channels |
| Robustness | J301 → DK; **daemon stopped** (`esw stop`) | `robustness_check.py [port]` | 14/14 (2026-11-09): reset-halt, panic with SWD closed, host loss mid-session, REBOOT, CRC catches a corrupted word |

The cross-cabled UART headers of section 4 conflict with this wiring: re-cable channel A to the DK first.

## 6. What a release needs to pass
1. Section 1 (CI does it).
2. Section 3 on the target board.
3. Whatever DUT bench is available: section 4 or 5. Write the results into `docs/hardware-reference.md` section
   12 and, if they changed, into the tables here.

# Project status (2026-11-11)

The handover page: what exists, what was decided, what is known to be weak, and what comes next. Hardware tests
and their last results: `docs/testing.md`. Using the tools: `host/python/README.md`.

## Where it stands
- **Hardware:** board r5 (design revision d, JLCPCB, 2026-10-23), frozen. Two boards built. Board 2
  (`F0F5BD57EF34`) runs the firmware; board 1 works on USB but has no esp-swd firmware yet. ESP32-S3-WROOM-1-**N4**
  modules: 4 MB flash, no PSRAM.
- **Software:** firmware **0.5.0**, protocol **5**, host package `esp-swd` **0.5.0**, all on `main`.
- **MVP** (scope set by the user on 2026-11-11), done and tested on hardware:

| Requirement | How | Evidence |
|---|---|---|
| Two SWD channels; two DUTs flashed at once | one SWD worker per channel and CPU core; `esw flash A=... B=...` | nRF5340 1 MB + STM32U585 1.2 MB in 5.7 s (8.6 s one after the other) |
| Two UARTs, up to 1 Mbaud each (not necessarily while flashing at full speed) | DMA back-end on one channel, hardened interrupt back-end on the other | real pads, A ↔ B: byte-exact both ways at once up to 2 Mbaud; 1 Mbaud also during a dual flash |
| Isolation control: semi-automatic and direct from Python | interlock + holders; switch modes `auto`/`on`/`off` (`esw mode`, `Esw.mode`) | all modes checked on real pads |
| Firmware update | `esw update` = esptool over USB-Serial-JTAG (the peripheral resets into the ROM downloader in hardware) | done repeatedly, also through the running daemon |
| Two TTY endpoints for standard tools | `esw serve`: PTYs `ttyA`/`ttyB` | pyserial, stty/cat/echo, socat; baud from the program |
| Custom Python tool for flashing | `esw` CLI and the `esw.Esw` library | used for every test above |

- **Target modules:** nRF52 (DK-tested), nRF5340 application and network core, STM32U575/585 (internal flash,
  TrustZone off, RDP 1 → 0 unlock), generic Cortex-M fallback (identify and raw access only).

## Decisions taken
- **USB stays on USB-Serial-JTAG for now** (2026-11-11). It keeps esptool's hardware reset into the ROM
  downloader, so an update recovers even a hung firmware with no button and no GPIO0 short.
  - Cost: no native USB CDC ports; the TTYs come from the host daemon instead, so they are Linux-only.
  - Cost: the link is shared and capped at about 485 KB/s host → board, about 780 KB/s both ways together.
  - USB-OTG (TinyUSB) is for a later revision; `CLAUDE.md` holds the recovery rules that move must follow.
- **The board enforces isolation, never the host.** No switch mode, command or daemon setting closes a switch
  without VTref inside 2.8–3.6 V. Every boot starts isolated. Host loss (3 s + 2 s) releases host holds.
- **One owner per channel resource** in the daemon: SWD, and UART (stream + group hold together). The TTY bridge
  owns a UART unless its policy is `off`.
- **DUT firmware belongs to the user:** back up before any write, ask first, restore afterwards. Backups never go
  into the repository.

## Known limitations and risks
- **Link bandwidth:** parallel flashing and UART traffic share USB-Serial-JTAG. With both UARTs at 1 Mbaud full
  duplex, a dual flash takes about 11.5 s instead of 5.7 s.
- **TTYs:** Linux only (PTYs, Unix sockets). Parity comes from `esw serve --format-a/-b` (Linux PTYs cannot carry
  it). No DTR/RTS/break (the r5 header has none). Mode `on` lasts while the host session lasts: with the daemon,
  until changed or the daemon stops.
- **Test hooks are always built in** (`ESW_TEST_HOOKS` in `app/main/CMakeLists.txt`). Any host can send the
  deliberate-panic (0x7E) and flash-stress (0x7D) commands. A production build should drop them.
- **Board USB RX ring (8 KB, no flow control):** host budgets assume the board's dispatcher keeps reading. A
  dispatcher blocked in `send_frame` (up to 500 ms under heavy UART RX while the host stalls) could in theory let
  the ring overflow. A frame is then lost: the daemon notices it within a second and fails that request. This was
  flagged in the 2026-11-11 review and has not been reproduced.
- **nRF5340:** only images with Nordic's startup code. A random or half-written image locks the debugger; recovery
  is ERASEALL, which also wipes the KMU keys. The DUT's firmware rewrites flash page 0xED000 at run time (settings),
  so its backups age.
- **Not supported yet:** STM32U5 with TrustZone on, external-flash loaders, SWO, a soak of more than an hour.

## Next steps (suggested order)
1. **Regression on the nRF52840 DK** with firmware 0.5.0: `docs/testing.md` section 5 (`uart_test`, `uart_stress`,
   `robustness_check`, `nrf52_blinky`, `flash_speed`, `swd_bench`).
2. **Soak test:** hours to days of repeated dual flashing plus UART streaming, through the daemon, with board
   reboots and USB re-plugs in between. Count every failure. (A script for this does not exist yet.)
3. **Production build:** a CMake option or Kconfig switch for `ESW_TEST_HOOKS`, off in release builds.
4. **Releases:** tag `v0.5.0`. Attach the merged image (`idf.py merge-bin`) so a board can be updated without
   ESP-IDF: `esw update esp-swd-0.5.0.bin`.
5. **Second board:** flash board 1 (`esw update` or `idf.py flash`) and run `docs/testing.md` section 3 on it.
6. **Later revision:** USB-OTG with TinyUSB (native CDC TTYs, vendor bulk at about 1 MB/s, optional CMSIS-DAP v2),
   OTA over the protocol, STM32U5 TrustZone, external-flash loaders on the streaming ABI, an nRF5340 streaming
   loader, SWO, and the host daemon on Windows/macOS.

## History
| Date | Milestone |
|---|---|
| 2026-10-21 … 10-23 | r5 schematic and layout, JLCPCB order (design revision d) |
| 2026-11-06 | boards built; MicroPython board test; C library + ESP-IDF firmware; nRF52840 DK flashed end to end |
| 2026-11-09 | public on GitHub; dedicated-GPIO SWD engine (1 MB in 3.4 s); UART bridge with DMA |
| 2026-11-10 | nRF5340 and STM32U5 modules; STM32U5 streaming loader; channels in parallel (protocol 4) |
| 2026-11-11 | MVP: switch modes, `esw` CLI, `esw serve` daemon with TTYs, firmware 0.5.0 (protocol 5), real-pad UART tests |

Details per milestone: `docs/firmware-architecture.md` (section 9) and `docs/hardware-reference.md` (section 12).

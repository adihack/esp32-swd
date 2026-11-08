# esp-swd: firmware architecture

**Status:** agreed direction, 2026-11-06. Decisions are in section 10. Inputs:
- the r5 SWD board test (`firmware/poc_swd`, which passed on an nRF52840);
- the owner's requirements:
  1. a portable, modular library, kept separate from the board and the MCU;
  2. a custom Python host application that manages SWD, the UARTs (including sniffing) and the TI isolation switches;
  3. speed and reliability: the tool stays plugged in for weeks with no replugging (replacing J-Links);
  4. generic RAM loaders, for example for the STM32U5's external flash, over a channel faster than J-Link's.

Project name: **esp-swd**. Repository: <https://github.com/adihack/esp32-swd>, MIT licence (`LICENSE`).

## 1. Layers
```
 host/python  (pip package: library + CLI, UART PTY bridges, ELF/loader tooling)
      │ USB: vendor interface (always) + optional CMSIS-DAP v2 / CDC-ACM; later TCP. Same wire protocol everywhere
 ─────┼────────────────────────────────────────────────────────────────────────────────────────────────────
 app/          ESP-IDF application: tasks, wiring, OTA, watchdogs, USB composition            (per MCU family)
 boards/r5/    pin map, which switch group sits on which pin, VTref divider, limits            (per board)
 ports/esp32s3 SWD PHY, UART+DMA, ADC, GPIO, USB descriptors, flash/NVS                         (per MCU)
 lib/          portable C11, no RTOS, no vendor headers
   core/       COMMON STACK, chip-independent:
               swd/     SWD PHY interface + portable bit-bang engine; ADIv5 DP/AP (DPv1/DPv2), MEM-AP incl. blocks
               cpu/     core-control ops (halt, resume, registers, reset); Cortex-M implementation first
               loader/  generic RAM loaders for ANY chip: download, execute, mailbox channel (section 5)
               target/  target-module interface + registry (detect, memory map, unlock, erase/program/verify)
   targets/    EXTENDABLE chip/family modules: nrf52, nrf53, stm32u5, … (each self-contained, registered in a table)
   channel/    channel state machine and isolation interlock (hardware-agnostic)
   proto/      framing (COBS + CRC-16), commands, stream multiplexing, versioning
 loaders/      target-side RAM code: streaming-loader SDK (Cortex-M) and FLM adapter
```
**Rules:**
- **Op tables only.** `lib/` reaches hardware only through small op tables that the port and board supply:
  `swd_phy`, `switch_ops`, `vtref_ops`, `clock_ops`.
- **No dynamic allocation.** `lib/` never allocates; the caller owns every buffer.
- **Host-tested.** `lib/` builds and is unit-tested on a PC, in CI, against a **simulated SWD target** (a C port of
  the bit-level emulator used for the board test).
- **One port today.** The current target is the **ESP32-S3** (`ports/esp32s3`). Any other host MCU is a new `ports/`
  and `boards/` directory. Nothing outside `ports/` may depend on the CPU architecture or vendor SDK.
- **Chip support is a module.** A target module implements the `esw_target_driver` interface:
  - detect and identify;
  - memory regions;
  - unlock or recover;
  - erase, program and verify;
  - reset.

  For each memory region it chooses a method: *direct* (the module drives the chip's flash controller over MEM-AP), or
  *loader* (it hands a loader descriptor to the common loader runtime). Adding a chip means adding a directory under
  `lib/targets/` plus one registry line; the common stack stays untouched.
- **Packet-level PHY.** The SWD PHY interface is a *packet* interface: request, ACK, data, plus raw sequences and
  optional block streaming. That way a fast engine (dedicated GPIO, SPI+DMA, or another MCU's peripheral) replaces the
  portable bit-bang engine without touching the ADIv5 layer.

## 2. Isolation and channels (lib/channel)
Per channel, states follow the brief:
- ISOLATED (default);
- VTREF_SENSE;
- CONNECTED_SWD, CONNECTED_UART, or both.

Every transition goes through one interlock, the sequence proven on r5:
1. VTref switch first.
2. Wait for the settle time.
3. Check the voltage window.
4. Put the tool-side lines in a safe state.
5. Close the signal group.
6. Keep monitoring VTref; if it drops, open everything.

On disconnect: debug off, then pin reset, then the signal groups open, VTref last.
- The host **requests** states. Only the firmware **enforces** the interlock.

**Holders and host loss (default: isolated).** A signal group stays closed only while it has a holder:
- the **UART group** while that UART is open on the host: its stream subscribed in the vendor protocol, or DTR
  asserted on a CDC port;
- the **SWD group** while a programming job runs or a CMSIS-DAP session is connected (DAP_Connect until
  DAP_Disconnect).

When the last holder goes, the group opens. The VTref switch opens when no group is held. Host loss releases every
holder after a short grace period (about 2 s), so the channel returns to ISOLATED. Host loss means any of:
- USB suspend or bus reset;
- the vendor-session heartbeat stops;
- the host app closes the port.

The board is USB-powered, so "powered without a host" means the PC is asleep or the app is gone. In that case the
channel isolates.

Every state change is an event on the event stream, with a reason (holder change, VTref lost, host loss, error).

## 3. Host link (USB)
**Vendor interface (always present).**
- It is the management path and the fast data path: WinUSB/libusb bulk, with MS OS 2.0 descriptors so Windows needs
  no driver.
- It carries multiplexed **streams**:
  - `ctrl`: request/response, sequence numbers, idempotent commands;
  - `event`: VTref, state changes, errors;
  - `uartA`, `uartB`: data in and out, with device timestamps on RX chunks for sniffing;
  - `job`: programming and loader jobs, plus image upload.
- The Python library exposes each UART as a **PTY** (e.g. `/dev/esp-swd/<serial>/uartA`), so picocom and log scripts
  work unchanged.

**Optional interfaces.** Each can be enabled or disabled from the host. The setting is persisted in NVS and applied
by a USB soft re-attach, so the PC re-enumerates and no replug is needed.
- **CMSIS-DAP v2:** OpenOCD, pyOCD, probe-rs and GDB work unchanged. It goes through the same channel interlock.
- **CDC-ACM per UART:** native serial ports, where the endpoint budget allows.

**ESP32-S3 endpoint budget.** USB 2.0 Full Speed, about 9 Mbit/s usable.
- Espressif documents 6 endpoints besides EP0, with a limit on simultaneous IN endpoints.
- Budget per function: vendor 2, CMSIS-DAP 2, each CDC-ACM 3.
- The firmware checks the requested composition against the budget and refuses an impossible one.
- Which compositions fit (for example vendor + CMSIS-DAP, or vendor + 1 CDC) is measured on r5 in M1. The vendor
  interface alone always carries everything.

## 4. Programming path and speed
- **Programming runs on the tool.**
  1. The host uploads the image (and any loader) once, cached by hash.
  2. The tool erases, writes and verifies at target-memory speed.
  3. USB latency is out of the loop.
  4. With one SWD engine per channel, both channels program **in parallel**.
- **nRF52/nRF53 internal flash.**
  - Direct NVMC writes over SWD reach the flash limit: 41 µs per word, so about 10.7 s per MB.
  - The link with address auto-increment needs about 48 clocks per word: 12 µs at 4 MHz, 6 µs at 8 MHz.
  - A loader is used only for CRC verify, at about 0.2 s/MB against about 3 s/MB to read back at 4 MHz.
- **SWD PHY on the ESP32-S3: dedicated-GPIO packet engine, implemented and measured (2026-11-06).**
  - Code: `ports/esp32s3/esw_dgpio.c`. It uses `ee.wr_mask_gpio_out` / `ee.get_gpio_in`, paces edges by CCOUNT, runs
    from IRAM with interrupts masked per packet, and fences the SWDIO output enable over APB at the turnarounds.
  - The packet sequencer, `ports/esp32s3/dgpio_packet.h`, is pure C and is tested in CI against the simulator.
  - Measured: 7.37 µs per packet at 8 MHz (bit-bang 31.3 µs), 0 errors in about 3 M packets at 8 MHz and at 10 MHz.
  - A full 1 MB nRF52840 erase + program + CRC verify takes 3.42 s.
  - Engines are bound to core 1. They are created at boot and re-routed and self-checked at every attach, because
    board pin setup re-routes the pads.
  - Parked: SPI + GDMA with ORUNDETECT. At or below 10 MHz the wire time dominates, so it would gain little. nRF52
    programming is paced by WAITs, which ORUN streaming cannot take.
  - Next: one engine per core, so both channels run in parallel.
- **Verify by on-target CRC.** A built-in CRC-32 loader for any Cortex-M (`loaders/crc32/`, 164 B ARMv6-M Thumb,
  position independent, needs 4 KB of scratch RAM per target module) checks 1 MB in 0.17 s, instead of 2.7 s
  read-back. The host compares it with `zlib.crc32`. Read-back is the fallback.
- **UARTs** use the UART peripheral with DMA (UHCI/GDMA) and ring buffers; the target is ≥ 1 Mbaud on both channels
  at once.

## 5. Target-side loaders (generic, fast)
Purpose:
- program memories the probe cannot write directly, first of all the STM32U5's **external (O/HSPI) flash**;
- verify by CRC on the target;
- speed up internal flash where a part's programming mode needs gap-free writes.

Loader support is **part of the common stack** (`lib/core/loader`) and works for any chip with RAM and a supported
core. Chip modules only supply descriptors. The runtime:
- downloads the loader into target RAM (MEM-AP block writes);
- executes it through the core-control ops (`lib/core/cpu`, Cortex-M first, architecture-neutral interface);
- runs the mailbox communication channel.

It supports two ABIs.

**a) Streaming loader: esp-swd native ABI, the fast path.**
- The loader is started once and then runs a resident loop in target RAM. It polls a **mailbox** and a **ring of data
  buffers**:
  ```
  mailbox: magic, abi_version, caps, cmd_seq, done_seq, cmd, addr, len, buf_index, status, result, crc
  ring:    N buffers × B bytes, each with a state word FREE → FILLED → BUSY → DONE
  ```
- Commands: INFO (geometry, buffer layout), ERASE(range), CHIP_ERASE, PROGRAM(addr, buffer), READ(addr → buffer),
  CRC32(range), BLANK_CHECK(range), EXIT.
- The probe fills free buffers with MEM-AP block writes **while the core runs**. It polls status words with reads and
  never halts the core per page.
  - SWD transfer and memory programming overlap, so throughput ≈ min(SWD link, memory write speed).
  - There is no halt/resume or register round trip per page. That per-call halt/resume cycle is what this ABI removes.
- `loaders/sdk/` holds the runtime: startup, mailbox loop, CRC, linker-script template, and an `arm-none-eabi-gcc`
  build.
  - Porting an existing loader means plugging its device functions (init, erase, program, read) into the template.
  - The flash-driver code of the current J-Link loaders carries over; only the entry glue changes.

**b) CMSIS flash algorithm (FLM): the compatibility path.**
- It runs CMSIS-Pack algorithms, the format SEGGER's Open Flashloader is based on: Init, UnInit, EraseSector,
  EraseChip, ProgramPage, and optionally Verify or BlankCheck.
- Call mechanism, per call:
  1. halt the core;
  2. load R0–R3, R9 (static base), SP, LR = a `BKPT` stub and PC;
  3. resume;
  4. wait for the halt;
  5. read R0.
- Page buffers are double-buffered: the next page is written into RAM while ProgramPage runs.
- An optional `loaders/flm-adapter` links an FLM's sources with the streaming runtime, which turns it into a streaming
  loader by recompiling.

**Common rules.**
- The host parses the ELF (Python) and sends a normalized **loader descriptor**:
  - segments and load address;
  - entry offsets;
  - RAM regions for code, stack, mailbox and ring;
  - device geometry;
  - per-command timeouts.
- The tool has no ELF parser.
- Every loader command has a bounded timeout from the descriptor.
- Before a loader starts, the tool freezes target watchdogs in debug where the part allows (for STM32, DBGMCU freeze
  bits); otherwise the loader kicks them.
- Results and errors come back as status codes and events. A hung loader means halt, report, then reset the target.
- **STM32U5 notes.**
  - Cortex-M33 with DPv2 and TrustZone.
  - With TZEN set, the driver selects secure or non-secure bus access (the CSW HNONSEC bit) and checks RDP before
    programming.
  - Internal flash is programmed directly or by loader; external flash needs a loader.

## 6. Reliability: weeks plugged in, never a replug
| Risk | Measure |
|---|---|
| Firmware hang | Task watchdog on every task, interrupt watchdog, brown-out reset. Every wait is bounded; no `portMAX_DELAY` on I/O |
| Crash with switches closed | A panic resets only the CPUs, so the GPIO latches survive. Three layers drive every enable low: `__wrap_esp_panic_handler` (first action in the panic handler), a shutdown handler (every `esp_restart`), and a 2nd-stage bootloader hook (`bootloader_before_init`) |
| Leaks or fragmentation over weeks | No heap allocation after init (FreeRTOS static allocation); fixed pools; free-heap and stack watermarks exported |
| USB stack wedged (as with MicroPython on 2026-11-06) | SOF/activity supervisor. Recovery is a soft re-attach (the host re-enumerates), then a watchdog reset. The DUT switch state comes from the interlock and holders, not from the host |
| Host app crash or restart | Holders are released after the grace period, so the channel isolates. The host re-reads state on reconnect. No hidden state in the host |
| Target trouble (WAIT storms, a dead or locked target, a hung loader) | Bounded retries, line reset and re-connect, sticky-error clear, VTref-loss abort, loader timeouts. Errors return codes and never stall the engine |
| Bad firmware update | Two OTA slots with rollback (ESP-IDF app rollback), plus the "reboot to ROM download" command required by `CLAUDE.md` (no buttons). Under USB-OTG it needs the OTG persist flags or a PHY remap (hardware-reference.md section 8). It is bench-tested before any OTG image ships |
| Linux host side | udev rule: stable name by USB serial, `ID_MM_DEVICE_IGNORE` (ModemManager), USB autosuspend off for this VID:PID |
| Unknown long-term failures | Persistent counters (resets with reason, USB re-attaches, SWD errors, uptime) read through `ctrl`; soak rig (section 7) |

MicroPython stays a bring-up tool only. It hung the native USB twice in one evening.

## 7. Verification
- **CI on a PC:**
  - `lib/` unit tests against the simulated target: SWD protocol, DP/AP, MEM-AP, nRF52 flash, the loader manager
    with a simulated loader, the interlock and holders, protocol fuzzing;
  - host package tests against a device emulator.
- **Hardware-in-the-loop rig:**
  - an r5 board plus an nRF52840 DK and an STM32U5 board with external flash, on a Linux host, around the clock;
  - each loop: program and verify (internal and external flash), UART echo at 1 Mbaud, isolation cycling;
  - fault injection: USB reset/unbind, host-app `kill -9`, DUT power cycling.
  - Acceptance: 14 days with no replug and no unexplained error.

## 8. Repository layout
```
lib/  ports/esp32s3/  boards/r5/  app/  loaders/  host/python/  tests/{sim,hil}/  hardware/rev5/  docs/
firmware/bringup, firmware/poc_swd   (MicroPython bring-up and board test; kept as reference)
```

## 9. Milestones
1. **M0:**
   - common core: SWD PHY interface, bit-bang engine, ADIv5, Cortex-M core ops, target interface and registry, generic
     loader runtime;
   - nRF52 as the first target module;
   - channel interlock;
   - simulated target in C, which can also emulate loader code;
   - host build and tests, CI.
2. **M1:**
   - ESP-IDF app on r5: safe-state boot, watchdogs;
   - TinyUSB vendor interface, protocol framing;
   - isolation, holders and VTref control from Python;
   - OTA, and reboot to ROM;
   - endpoint-budget measurements.
3. **M2:** fast SWD PHY (benchmark both options), nRF52840 program and verify via Python, measured against
   10.7 s/MB.
4. **M3:** UART streams with DMA, timestamps, PTY bridges (sniffing). Optional CDC-ACM.
5. **M4:**
   - loader manager with the streaming ABI and SDK, plus the FLM path;
   - STM32U5 internal flash and external flash via loader;
   - optional CMSIS-DAP v2.
6. **M5:** nRF5340 (app and net cores), parallel channels, soak rig and the 14-day run.

## 10. Decisions (2026-11-06)
1. **Host MCU:** ESP32-S3 now. The layers keep any other MCU possible; the ESP32-S31 was an example, not a target list.
2. **CMSIS-DAP v2:** nice to have; it can be enabled or disabled from the host (section 3).
3. **Channel A target:** STM32U5xx.
   - It needs loaders for external flash. The existing J-Link loaders will be rebuilt as needed.
   - esp-swd provides generic loader support: run from RAM, over a faster channel than J-Link (section 5).
   - Loader support is part of the common stack and works for any chip. Exact chip or family support is an
     extendable module (section 1).
4. **Project:** name esp-swd; MIT licence, public at <https://github.com/adihack/esp32-swd> (2026-11-09).
5. **Default:** isolated. Groups stay closed only while a UART or SWD/DAP holder is open (section 2).

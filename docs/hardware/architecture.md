# Dual SWD Flasher + UART Bridge — Phase 1 Architecture

Source brief: `design-brief.md`. Decisions made 2026-10-21.

## Resolved open questions
| Question | Decision |
|---|---|
| UART count | Channel A = 1 UART, Channel B = 1 UART (native UART1/UART2; UART0 unused, no SC16IS752) |
| SWO | Included in hardware on both channels (fills the 4th SWD-group switch) |
| Connectors | Per channel: ARM Cortex 10-pin 1.27 mm SWD header + 1×4 2.54 mm UART header |
| DUT power | nRESET only in Phase 1. Nothing sits in the measured supply path |
| PCB | 2-layer, JLCPCB, 0.15/0.15 mm, autorouted via Freerouting |

## Revision 2026-10-22: fixed 3.3 V DUT I/O
The user confirmed both DUT MCUs always run their I/O at 3.3 V. The tool therefore drives the DUT directly at its own
3.3 V level through the isolation switches:
- Removed per channel: 6× SN74AXC1T45 + 12 caps, the VIO block (TLV9061, TS5A3159A, TPS7B4250, PWM RC filter,
  VIO divider, VIO_SEL pull-down, 10M DNP), the SWDIO DIR pull-down, and the VIO test point.
- Kept: TMUX1511 isolation (leakage requirement unchanged), ESD321 on every DUT line, 33 Ω on SWCLK/SWDIO, 2N7002
  nRESET driver + direct readback, EN pull-downs, tool-side pulls (now to +3V3), LEDs, connectors, test points.
- VTref: TMUX → 100k/100k divider → ESP ADC. It only detects whether the DUT is powered; no tracking.
- Freed ESP32 GPIOs: A/B_VIO_ADC, A/B_VSET_PWM, A/B_SWDIO_DIR. Freed expander outputs: P0_3, P1_3 (were VIO_SEL).
- +5V now only feeds the 3.3 V LDO (AMS1117-3.3 since rev (c)) and the Phase-2 header.
- Interlock (firmware): close signal switches only after VTref reads present (≥ ~2.8 V); otherwise the tool's 3.3 V
  back-powers an unpowered DUT through the signal lines and tool-side pull-ups.
The sections below describe the original 1.6–3.6 V design; where they conflict, this revision wins.

## Revision 2026-10-22 (b): layout-driven pin assignment
Changed to make every signal lane escape without crossings on the 2-layer board (no electrical change):
- **TMUX1511 roles swapped:** tool side on **Sx**, DUT side on **Dx** (the datasheet specifies the path as bidirectional,
  with identical IS(OFF)/ID(OFF) and IPOFF limits for S and D). With TSSOP-14's SEL-S-D-SEL-S-D pin order this lets S1/S4 escape
  up under the package body, S2/S3 up the outside, D1/D4 down under the body and D2/D3 down the outside. The DUT-side order then
  matches the connector exactly: SWDIO, SWCLK, SWO, NRST (J301) and TX, RX, VTREF (J302).
- **SWD switch (U301/U401):** sw1 SWCLK, sw2 SWDIO, sw3 NRST, sw4 SWO (unchanged assignment, S/D swapped).
- **AUX switch (U302/U402):** sw1 UART_RX + sw2 UART_TX (SEL1/SEL2 = EN_UART), sw3 VTREF (SEL3 = EN_VTREF), sw4 spare
  (SEL4 pin 13 NC, so the internal 6 MΩ pull-down keeps it open; S4/D4 NC).
- **ESP32 GPIO map re-assigned** so the module pins leave in the same west→east order as the channel lanes (table below).
- **I2C SDA/SCL swapped** between IO40 and IO41 (GPIO-matrix choice), so the bus reaches the expander without crossing.

## Revision 2026-10-22 (c): JLCPCB cost reduction
Extended LCSC part types went from 12 to 6 (JLC charges a loading fee per Extended type).
- F101 polyfuse → **0 Ω 1206 link**. The USB host port limits current; the 1206 footprint still takes a PTC.
- FB101 → 100 Ω @ 100 MHz 0805 bead; USB ESD U101 → **SRV05-4** (same SOT-23-6 pinout as USBLC6-2SC6).
- Green LEDs → **white** (pure green, Vf about 3 V, is too dim from 3.3 V). R103 → 1 kΩ, R323/R423 → 330 Ω.
- SW201 RESET, SW202 BOOT, SW203 START not assembled (superseded by revision (d): removed from the design).
- **U102 AP7361C-33E → AMS1117-3.3 (Basic).** AMS1117 pinout is 1 GND, 2/tab VOUT, 3 VIN (AP7361C: 1 VIN, 2/tab GND,
  3 VOUT); the KiCad symbols keep VI/GND/VO at the same positions, so only the board changed: U102 rotated 180°, +5V into
  VIN straight from FB101, the 3V3 tab onto C104/C105. Dropout 1.1 V at 800 mA (enough from 5 V USB). C104 10 → 22 µF,
  the output capacitance the AMS1117 datasheet asks for.
- **SWD header J301/J401 → C3975188 (HCTL PZ127, male).** The earlier C3975131 (PM127) was a female socket.
- A THT 1.27 mm header is Extended too and adds THT assembly, so the SMD one stays. The ESD321 stays because
  Basic/Preferred ESD diodes leak 100–500 nA each (brief: DUT leakage).

## Revision 2026-10-22 (d): no expander, one LED, no buttons
User decision: "not a christmas tree": at most one LED, no buttons. JLC's economic PCBA does not place the ESP32 module,
so U201 is hand-soldered. Removed: PCAL6416APW (U202) + C204/C205, I2C pull-ups R205/R206, XP_RSTn pull-down R207,
the six channel LEDs D308–D310/D408–D410 + R323–R325/R423–R425, SW201/SW202/SW203 and START pull-down R204 (22 parts).
- **Isolation enables are driven directly by ESP32 GPIOs.** Each pin used resets as an input with no internal pull
  (datasheet Table 2-1 "At Reset": IE only) and has at most a *low* power-up glitch (Table 2-2), so the existing 100 kΩ
  EN pull-downs hold every switch OPEN through power-up, reset, brown-out and the ROM bootloader, which satisfies the
  brief's "OPEN whenever firmware is not running". GPIO18 is avoided for enables: it has a 60 µs *high* power-up glitch.
  GPIO46 is a strap pin with an internal pull-down at reset, so low is its natural state.
- Pins were picked for the board: each GPIO drops straight onto its pull-down in the centre row
  (R301 A_EN_SWD … R401 B_EN_SWD); the two old I2C tracks and the old XP_RSTn path were reused for channel B.
- **D101 is the single LED, now a firmware status LED** (LED_STATUS = GPIO2 → R103 1 kΩ → D101 → GND, active high).
  It is off while firmware is not running.
- No RESET/BOOT buttons. EN keeps its RC (R201/C203); GPIO0 keeps its pull-up (R202) for normal SPI boot. Download mode
  is entered from firmware (USB command → under USB-OTG, OTG persist flags or a PHY remap to USB-Serial-JTAG, see hardware-reference.md section 8 → set RTC force-download flag → restart) or with esptool when USB-Serial-JTAG is
  active. A firmware that disables USB early needs GPIO0 pulled low at power-up: module pad 27 (castellated) can be
  shorted to GND by hand.
- Extended LCSC types in the JLC order: TMUX1511PWR, ESD321DPYR, USB-C receptacle, 1.27 mm 2×5 SMD male header (4).

## Key part choices (datasheet-verified)
| Function | Part | Why |
|---|---|---|
| Controller | ESP32-S3-WROOM-1-N16 | per brief |
| Isolation switch | **TMUX1511PWR** (4× SPST, TSSOP-14), 2 per channel | SELx active-high with internal 6 MΩ pull-down → OPEN by default; powered-off protection to 3.6 V; IS(OFF) 0.03 nA typ (25 °C); CON 3.3 pF; RON 2 Ω; signals up to 2×VDD (VDD = 3.3 V) |
| VIO regulator | **TPS7B4250-Q1** tracking LDO (SOT-23-5), 1 per channel | VOUT tracks ADJ/EN within ±5 mV for 1.5–18 V; standby when ADJ < 0.8 V; 50 mA |
| VTref buffer | **TLV9061** (SOT-23-5) from +5 V, 1 per channel | pA input bias → VTref load ≈ switch leakage only |
| VIO reference mux | **TS5A3159A** SPDT (SOT-23-6) from +5 V | IN low → NC (VSET, default); IN high → NO (VTref). VIH 2.4 V at 5 V supply → 3.3 V logic OK |
| Translators | **SN74AXC1T45DBVR**, 6 per channel | dual supply, Ioff, 0.65–3.6 V B side covers 1.6 V; no auto-direction |
| nRESET driver | 2N7002 open-drain + translator readback | never driven high |
| DUT ESD | **ESD321DPYR** (alt TPD1E01B04DPYR, same footprint) | GND-referenced, 0.1 nA typ, 0.9 pF |
| 3.3 V | AMS1117-3.3 (1 A, SOT-223; was AP7361C-33E, rev (c)) | headroom for Phase 2 RF peaks; JLC Basic part |
| USB ESD | SRV05-4 (was USBLC6-2SC6, same pinout) | host side only |

## Signal chain per channel (x = A or B)
```
DUT conn ─ ESD321 ─ x_*_D ─[TMUX1511]─ x_*_T ─ 33Ω (SWCLK/SWDIO) ─ AXC1T45 (B=VIO_x, A=3V3) ─ ESP32
VTref: x_VTREF_D ─[TMUX1511 ch, EN_VTREF]─ x_VTREF_T ─ TLV9061 follower ─ x_VTREF_BUF ─┬─ 2:1 divider → ADC
                                                                                        └─ TS5A3159A NO
VSET:  ESP PWM ─ 2-stage RC ─ x_VSET ─ TS5A3159A NC (default)
TS5A3159A COM ─ 1k/100nF ─ TPS7B4250 ADJ/EN ─ VIO_x ─ 2:1 divider → ADC
```
Switch groups per channel: SWD = {SWCLK, SWDIO, nRESET, SWO} (TMUX1511 #1, SEL1–4 tied → x_EN_SWD);
AUX = {VTREF (SEL1 → x_EN_VTREF), UART_TX + UART_RX (SEL2+SEL3 → x_EN_UART), ch4 spare}.
Tool side on Sx pins, DUT side on Dx pins (see revision (b)). Every EN line has an external 100 kΩ pull-down.

Safe defaults at power-up / reset / brown-out (rev (d)): the EN GPIOs reset as unpulled inputs → 100 kΩ pull-downs
hold every x_EN_* low → all switches OPEN until firmware drives them.

## ESP32-S3 pin map
Revised 2026-10-22 (b) for PCB layout: each channel's signals leave the module in the same west→east order as its
lanes (SWDIO, SWCLK, SWO, NRST_DRV, NRST_SENSE, UART_TX, UART_RX, VTREF). Channel A uses the module's left pads 4–12
and runs down the left corridor; channel B uses the bottom row (pads 19–25) and then the right side (pads 28, 39).
Module pad numbers are in brackets. Revision (d) moved the isolation enables and the status LED onto ESP32 pins.
GPIO 13, 18 and 36 are spare (no-connect); GPIO3 goes to test pad TP201.

| GPIO [pad] | Net | Notes |
|---|---|---|
| 0 [27] | BOOT_BTN | strap; 10k pull-up R202 (no button since rev (d)) |
| 2 [38] | LED_STATUS | status LED D101 via R103, active high |
| 9 [17] | A_EN_SWD | isolation enable, 100k pull-down R301 |
| 10 [18] | A_EN_VTREF | isolation enable, 100k pull-down R303 |
| 1 [39] | B_VTREF_ADC | ADC1_CH0 |
| 3 [15] | TP_GPIO3 | spare → test pad (strap: JTAG select, ignored unless eFuse set) |
| 4 [4] | A_SWDIO | dedicated-GPIO bundle |
| 5 [5] | A_SWCLK | dedicated-GPIO bundle |
| 6 [6] | A_SWO | input |
| 7 [7] | A_NRST_DRV | 100k pull-down |
| 15 [8] | A_NRST_SENSE | input |
| 16 [9] | A_UART_TX | UART1 TX |
| 17 [10] | A_UART_RX | UART1 RX |
| 8 [12] | A_VTREF_ADC | ADC1_CH7 |
| 11 [19] | B_SWDIO | dedicated-GPIO bundle |
| 12 [20] | B_SWCLK | dedicated-GPIO bundle |
| 14 [22] | B_SWO | input |
| 21 [23] | B_NRST_DRV | 100k pull-down |
| 47 [24] | B_NRST_SENSE | input |
| 48 [25] | B_UART_TX | UART2 TX |
| 35 [28] | B_UART_RX | UART2 RX (free on N16, no PSRAM) |
| 19 / 20 [13/14] | USB_D− / USB_D+ | USB-OTG (CDC-NCM) |
| 37 / 38 / 39 [30/31/32] | P2_SPI_MISO / MOSI / SCK | Phase 2 reserve |
| 40 [33] | B_EN_SWD | isolation enable, 100k pull-down R401 |
| 41 [34] | B_EN_UART | isolation enable, 100k pull-down R402 |
| 42 [35] | B_EN_VTREF | isolation enable, 100k pull-down R403 |
| 44 [36] | P2_W5500_INTn | Phase 2 reserve (U0RXD) |
| 43 [37] | P2_W5500_CSn | Phase 2 reserve (U0TXD prints ROM log at boot; W5500 is held in reset then) |
| 45 [26] | P2_W5500_RSTn | strap (VDD_SPI): 10k pull-down keeps it low at boot |
| 46 [16] | A_EN_UART | isolation enable, 100k pull-down R302 (strap; WPD at reset, low = normal) |

## Isolation enable map (rev (d), active high, 100 kΩ pull-down each)
A_EN_SWD GPIO9 · A_EN_UART GPIO46 · A_EN_VTREF GPIO10 · B_EN_SWD GPIO40 · B_EN_UART GPIO41 · B_EN_VTREF GPIO42
(the PCAL6416A expander of earlier revisions is removed).

## Power tree
USB-C VBUS → F101 (0 Ω link, PTC footprint) → ferrite → **+5V** → AMS1117-3.3 → **+3V3** (ESP32, TMUX1511)
+5V → TPS7B4250 ×2 → **VIO_A / VIO_B** (translators' B side, tool-side pulls); +5V also powers TLV9061 ×2 and TS5A3159A ×2.

## Known risks and firmware interlocks
- Leakage: the 7 DUT lines × (TMUX1511 IS(OFF) 0.03 nA typ + ESD321 0.1 nA typ) ≈ 1 nA typical, which meets the brief.
  Datasheet maximums (TMUX1511 100 nA over full temp; ESD 10 nA) exceed the 100 nA worst-case target, so the leakage test plan must confirm it by measurement.
- TPS7B4250 has an undefined region at 0.8–1.5 V on ADJ. Firmware must set VSET = 0 or ≥ 1.6 V only.
- Only select VIO_SEL = VTref after EN_VTREF is closed and VTref has been read as valid (the buffer input floats while open; a DNP 10 MΩ pull-down footprint is provided).
- Fixed VIO mode (1.8/3.3 V) with signal groups closed back-powers an **unpowered** DUT. Firmware must confirm the DUT is powered first.
- USB VBUS at its 4.4 V minimum still leaves enough TPS7B4250 headroom for VIO up to about 3.6 V at low current. Check during bring-up.

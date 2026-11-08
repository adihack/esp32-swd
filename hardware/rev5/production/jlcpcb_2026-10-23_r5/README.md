# JLCPCB package r5 — dual_swd_flasher (2026-10-23)

Source design: `dual_swd_flasher.kicad_pcb` sha256 `48f7cd4e42b1c0baf52156c8f8dacb101295b5c0711f9d883bb68479d689ccbd`
(KiCad 10.0.7 DRC with zone refill and schematic parity: 0 errors, 0 unconnected, 0 parity issues; ERC 0/0;
warnings are silkscreen overlaps and 3 library-sync flags).
The board file was saved again afterwards with only J101's 3D-model entry changed (sha `b68c9d79…`); Gerbers, BOM
and CPL are unaffected, so this package stands.
Design revision (d): no I/O expander, one status LED, no buttons (`docs/architecture.md`). Supersedes r4 (`../archive/`).

## Upload these three files
| File | Use |
|---|---|
| `dual_swd_flasher_gerbers_JLCPCB.zip` | PCB order: F/B copper, F paste, F/B mask, F/B silk, outline, PTH + NPTH drill (USB-C shell slots as G85) |
| `BOM-dual_swd_flasher_JLCPCB.csv` | PCBA "BOM" step (columns Comment, Designator, Footprint, LCSC Part #) |
| `CPL-dual_swd_flasher_JLCPCB.csv` | PCBA "CPL" step (Designator, Mid X, Mid Y, Layer, Rotation; mm, top side) |

`raw/` holds the unfiltered Konnect export (do not upload). The upload BOM/CPL are rebuilt from it with
`tools/manufacturing/jlc_upload.py raw .`.

## Order settings assumed for the quote
2 layers, FR-4, 49 × 45 mm, 1.6 mm, 1 oz outer copper, HASL lead-free (ENIG also fine), green mask, 5 pcs.
PCBA: economic, **top side only**, 74 SMD parts / 19 unique parts (Basic 14, Preferred 1, Extended 4).
Expected Extended loading fee: 4 × $3.09 = **$12.36** (JLC's live library data, 2026-10-23).
Parts about $4.90 per board, without the ESP32 module.

| LCSC | Part | Qty | Why it stays Extended |
|---|---|---|---|
| C2866750 | TMUX1511PWR (U301/U302/U401/U402) | 4 | the DUT isolation switch: low leakage, default-off |
| C2868952 | ESD321DPYR (D301–D307, D401–D407) | 14 | 0.1 nA leakage; Basic/Preferred ESD diodes are 100–500 nA each |
| C165948 | TYPE-C-31-M-12 USB-C (J101) | 1 | no Basic USB-C 16-pin receptacle |
| C3975188 | PZ127-2-05-S 1.27 mm 2×5 SMD male header (J301/J401) | 2 | no Basic 1.27 mm header |

SRV05-4 (C7420376, U101) is Extended with JLC's Preferred flag: no loading fee.

### Changes against r4
- I/O expander U202 (PCAL6416APW), its caps, I2C pull-ups and reset pull-down removed. The six isolation enables
  are now driven by ESP32 GPIO9/46/10 (channel A) and GPIO40/41/42 (channel B). The 100 kΩ pull-downs keep all
  switches open until firmware runs.
- Six channel LEDs and their resistors removed. D101 is now the single, firmware-driven status LED (GPIO2).
- All three buttons (and START's pull-down) removed from the design.
- U201 (ESP32-S3-WROOM-1-N16, C2913199) is no longer in the JLC BOM: economic PCBA does not place it. Hand-solder it.

## Not assembled by JLC (intentionally absent from BOM + CPL)
- **U201 ESP32-S3-WROOM-1-N16** (LCSC C2913199; must be the N16 *without* PSRAM, IO35 is used).
  - JLC still prints paste on its pads, so they arrive pre-tinned.
  - Castellated pads: iron. Centre GND pad: hot air or hotplate, or feed solder through the thermal vias from the
    bottom. Recommended for heat, but GND is also on the edge pads.
- J201: Phase-2 2×5 header, DNP.
- J302, J402: 2.54 mm 1×4 UART headers (through-hole, LCSC C2691448). Solder them by hand, or add THT assembly.
- TP101–TP407: bare test pads on the bottom side.

## CPL rotation corrections (applied by `tools/manufacturing/jlc_upload.py`)
Each part's EasyEDA footprint (what JLC places) was matched pad-by-pad against the KiCad footprint.
JLC rotation = KiCad rotation + offset:

| Parts | Package | Offset |
|---|---|---|
| U301, U302, U401, U402 | TSSOP-14 | +270° |
| U101 | SOT-23-6 (SRV05-4) | +270° |
| J301, J401 | 1.27 mm 2×5 SMD header | +270° |
| Q301, Q401 | SOT-23 | +180° |
| U102 | SOT-223 (AMS1117) | +180° |
| J101 | USB-C TYPE-C-31-M-12 | 0° (Konnect's automatic +180° was wrong and is undone) |
| everything else | LED, ESD321, passives | 0° |

D101 (C2290): EasyEDA numbers the cathode pad 2. Judge it by the **cathode mark**, not the pin-1 dot.
Check: every EasyEDA footprint placed per this CPL lands on the board copper within 0.3 mm (62 parts, 201 pads).

## Still look at JLCPCB's component-placement preview before paying
- J101 opening faces the left board edge.
- U102 tab on the left.
- Pin-1 dots of U101, U301, U302, U401, U402 sit on the silkscreen pin-1 marks.
- D101 cathode mark.
- J301/J401 pin 1.

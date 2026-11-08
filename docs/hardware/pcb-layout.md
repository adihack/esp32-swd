# PCB layout — design process and decisions

Board: **49 × 45 mm, 2 layers**, 95 parts, 794 track segments, 182 vias (after revision (d), section 9).
Final check (KiCad 10.0.7 DRC with zone refill and schematic parity): **0 errors, 0 unconnected items,
0 schematic/PCB mismatches**. The only warnings are silkscreen overlaps and library-sync flags, which are handled in the manufacturing step.

Images are in `docs/layout/`. The scripts that produced the layout are in `tools/layout/` (see "Reproducing" at the end).

## 1. Stackup and rules
| Item | Value |
|---|---|
| Stackup | 2 layers, 1.6 mm FR-4, 1 oz (JLCPCB standard) |
| Board minimums | track/space 0.15/0.15, via 0.6/0.3, hole-to-hole 0.25, copper-to-edge 0.5 |
| Default class | 0.2 track / 0.2 clearance, via 0.6/0.3 |
| `Power` class (VBUS, VBUS_F) | 0.4 track, via 0.8/0.4 |
| `Rail` class (+5V, +3V3) | 0.3 track (trunks drawn 0.4–0.5), via 0.6/0.3 |
| Layer convention | **F.Cu = vertical signal lanes, B.Cu = horizontal crossings, power trunks and the GND plane** |

The 2-layer stack was chosen over 4 layers for cost. Ground is two pours (top and bottom) that are stitched
together. Power is not a plane; it is a hand-designed copper skeleton (section 5).

## 2. Floorplan (`10_floorplan_v8.png`)
```
+-------------------------------------------------+
| LDO  5V chain |corr|    ESP32-S3 module    | J201 (P2 header, DNP)
| USB-C  ESD    |idor|  (antenna overhangs  | I2C pulls, LED
|               | A  |   the top edge)      | BOOT  RESET
+---------------+----+----------------------+------
| channel A (19.7 mm)   | expander  | channel B     |
|  tool zone: R / pulls | LEDs A|B  |  (same layout |
|  TMUX1511 x2          | EN pulls  |   translated) |
|  ESD row              | START     |               |
|  SWD 2x5  UART 1x4    |           |  SWD  UART    |
+-------------------------------------------------+
```
- **Antenna:** the ESP32 antenna overhangs the top edge, so there is no copper under it.
- **Channels:** the two channels are *identical copies* (channel B = channel A shifted 28.9 mm), so their layout and behaviour match.
- **Signal flow per channel** runs strictly top to bottom: ESP pin → series R / pull row → TMUX1511 isolation switch
  → ESD row → DUT connector on the front edge. Every DUT line is a straight lane, and the leakage-critical DUT-side
  copper is a few millimetres long.
- **UI:** originally a centre LED cluster driven by an I2C expander, plus BOOT/RESET/START buttons; revision (d) removed
  all of it, leaving one status LED (D101, top right) and a row of six EN pull-downs in the centre (section 9).
- **Test points** are on the bottom side, under the nets they probe (pogo-pin access).

## 3. Layout-driven schematic changes (no electrical change)
These were made because placement alone could not make the lanes crossing-free. All are documented in
`architecture.md` revision (b).
1. **TMUX1511: tool side on Sx, DUT side on Dx.** The package pin order is SEL-S-D-SEL-S-D. With the DUT on S, every DUT
   line has to cross a tool line. The path is bidirectional, and IS(OFF)/ID(OFF) and IPOFF are specified identically, so
   S and D can swap roles. After the swap, S1/S4 escape upward under the package body, S2/S3 upward along the outside,
   D1/D4 downward under the body and D2/D3 downward along the outside. The DUT-side order then matches the connector pins exactly.
2. **AUX switch reassigned:** sw1 = UART_RX, sw2 = UART_TX (both EN_UART), sw3 = VTREF, sw4 spare.
   The spare switch's SEL4 is left unconnected; its internal 6 MΩ pull-down keeps the switch open.
3. **ESP32 GPIO remap:** the module pins leave in the same west-to-east order as the channel lanes
   (SWDIO, SWCLK, SWO, NRST_DRV, NRST_SENSE, UART_TX, UART_RX, VTREF). VTREF_ADC stays on ADC1 (IO8, IO1).
4. **I2C SDA/SCL swapped** between IO40 and IO41 (a free GPIO-matrix choice), so the I2C lines reach the expander
   without crossing.

## 4. Placement rules applied
- Series resistors sit *in* their lane, with the ESP pad up. Pull resistors sit beside the lane, signal pad toward the switch.
- Decoupling capacitors sit at the TMUX VDD pin, with a planned 3V3 via under each one.
- ESD diodes are in connector order directly in the DUT lane. Each GND pad has its own pour connection.
- The two TMUX packages are 9.5 mm apart, leaving a 4-track gap for the SEL ties, NRST_SENSE and UART_TX.
- **Part flips for the power planning:**
  - The flipped resistors (5V cap, pull-ups, I2C/LED pull-ups) have their supply pad facing the rail or trunk that feeds it.
  - The EN pull-down row has its GND pads facing an open via row.
- The ratsnest was scored by counting crossings: 217 crossings went down to 126 with the final placement, and from 35 to 12 inside channel A.

## 5. Power distribution — hand-designed skeleton (`11_power_skeleton_early.png`)
Freerouting could never complete +3V3 (about 30 loads spread across the board), so power is hand-routed and locked.
- **3V3 source:** LDO VOUT → C104/C105 → a B.Cu hop under the 5V/VBUS chain → module 3V3 pad, together with C201/C202 and the EN pull-up.
  Since the switch to AMS1117 (tab = VOUT), U102 sits rotated 180°: +5V enters VIN directly from FB101, the 3V3 tab is joined
  to pin 2 by a 0.7 mm track under the body and drops straight onto C104/C105; pin 1 (GND) picks up a stitching via and
  C103's ground pad (`12_ldo_corner_ams1117.png`).
- **Trunks on B.Cu, run parallel to the signal lanes** so they do not cut the return paths of the vertical signals:
  - north trunk along the module's top edge;
  - right-column trunk;
  - left trunk along the module's left edge;
  - a centre trunk under the expander that feeds the LED resistor rail and START.
- **Channel bands:** one short horizontal B.Cu band per channel (y = 108.2). Each 3V3 load has a planned service via
  on the band. That gives one ground-plane cut per channel; the bottom plane stays joined around the centre column and the board edges.
- **5V:** FB101 → C101 → along the top edge to the LDO input. C102 and C103 join through short B.Cu spurs. A B.Cu run along
  the top edge carries 5V to the Phase-2 header.
- **VBUS:** both USB-C VBUS pad pairs and the USBLC6 VBUS pin are joined on a short B.Cu spine.
- **USB-C corner, hand-routed:**
  - D+ (A6/B6) is joined behind the pad row and D− (B7/A7) in front of it;
  - CC1 drops to B.Cu and CC2 goes straight out;
  - D+/D− cross the channel A bus on B.Cu.
- **Other planned paths (scarce corridors):**
  - EN runs from the module's left pad to RESET in the right column, under the module's top edge and down the right corridor;
  - I2C and XP_RSTn run as three parallel B.Cu lines under the module to the expander;
  - B_VTREF_ADC runs along the top edge and down a 1 mm lane east of J201;
  - the SWDIO_T edge lanes and EN_SWD bridges join the two SEL tie pairs of each SWD switch;
  - VTREF_D reaches SWD connector pin 1 on B.Cu behind the connectors.
- In total 249 locked items, verified at 0 DRC violations on their own before any autorouting.

## 6. Routing flow
1. **Freerouting 2.5.0** routes the signals around the locked skeleton.
   - GND is excluded; it is finished with pours.
   - F.Cu is vertical-preferred and B.Cu horizontal-preferred (cost 1.0/1.5 against 1.6–2.2/3–4), with via cost 35–60.
   - B.Cu under the module edges is allowed; the EPAD/thermal core is kept out.
   - Four rule variants run in parallel and the best result is kept.
2. **Grid maze router** (`maze.py`) routes the connections Freerouting leaves open. It works on KiCad's board model at
   0.1 mm resolution, two layers, 45° moves, with obstacles inflated by clearance plus half the track width (via size for
   via sites), and it keeps hole-to-hole clearance.
3. **GND pours** on both layers: SMD pads solid, THT thermal. The USB-C shield and the DNP header GND pins are solid.
   Stitching then goes in three stages:
   - **By area:** every pour fragment gets at least one via per 15 mm².
   - **Fragment and pad vias:** any fragment with no via, and any pad not yet connected, gets one.
   - **Islands:** ground islands are found with KiCad's connectivity engine and routed to main ground with short ground tracks.
4. **Rip-up and reroute** (`maze_rr.py`) for ground pads boxed in on both layers. The router may cross unlocked signal
   tracks at a high cost; it deletes the tracks it crossed and re-routes those nets, and keeps the change only if every
   affected net ends up whole.
5. **Cleanup** (`cleanup.py`) removes dangling stubs and stranded stitching vias, but only when connectivity is unchanged.
   Needed stubs are trimmed instead.

## 7. Ground
- Both layers are poured (0.25 mm clearance, 0.25 mm minimum width) and stitched. Of the 196 vias, 101 are GND,
  24 are power and 71 are signal.
- The module EPAD reaches the bottom pour through its 0.3 mm thermal vias.
- Every GND pad is connected. KiCad's connectivity reports a single GND island.

## 8. Known limitations / next steps
- **Cosmetic:**
  - A few EN control lines (A_EN_VTREF, B_EN_UART, B_EN_SWD) take long detours, left over from rip-up-and-reroute. They are
    static, slow signals, so this is electrically harmless, but they could be tidied by hand in KiCad.
  - The channel B centre and the expander area are the densest parts of the board.
- **Manufacturing (done 2026-10-22, `manufacturing/jlcpcb_2026-10-22_r5/`; older packages are in `manufacturing/archive/`):**
  - footprint attributes restored from the libraries, J201 set to DNP, and every part given an LCSC number;
  - Extended part types cut from 12 to 6 (architecture.md revision (c)); the three buttons are left unassembled;
  - JLCPCB Gerber/drill zip, BOM and CPL exported (SMT top side). Upload steps and checks are in that folder's README.
  - current package: `manufacturing/jlcpcb_2026-10-22_r5/` (revision (d) board).
- **Still open:**
  - silkscreen cleanup (reference overlaps, and text running past the right board edge);
  - a visual look at JLCPCB's placement preview (rotations are already corrected from measured EasyEDA footprint data,
    `tools/manufacturing/jlc_upload.py`).
- **Leakage:**
  - the DUT-side nets are short and on F.Cu, surrounded by GND pour;
  - wash the boards (no flux residue) as the brief requires;
  - confirm leakage by measurement (test plan).

## 9. Revision (d): expander, channel LEDs and buttons removed (`13_centre_no_expander.png`)
The schematic change (architecture.md revision (d)) removed 22 parts from the centre and right side. The re-route:
1. **KiCad F8** with "delete footprints with no symbols". KiCad moved the old I2C/START tracks onto the nets of the pads
   they touch; the LED copper became no-net.
2. **Prune dead copper** (`tools/layout/prune_conn.py`): inside a window, each track/via is removed and kept removed only
   if KiCad's connectivity shows every pad of its net still grouped the same. This removes dead stubs and branches, and
   degenerate zero-length segments, without guessing at geometry. (A naive "free end" test failed twice here: SWIG
   proxies are never `is`-identical, and dead stubs that overlap each other look anchored.)
3. **Pin choice for a crossing-free centre.** The six EN pull-downs sit in one row
   (R301 A_EN_SWD, R302 A_EN_UART, R303 A_EN_VTREF, R403 B_EN_VTREF, R402 B_EN_UART, R401 B_EN_SWD).
   - GPIO46, GPIO9 and GPIO10 (pads 16–18) drop nearly straight onto R302, R301 and R303. GPIO9 needs one B.Cu hop
     under A_EN_VTREF.
   - The two old I2C tracks (GPIO41/40) come down the right of the module side by side, so their roles were chosen to
     land on R402/R401 without crossing.
   - B_EN_VTREF uses GPIO42 along the old XP_RSTn B.Cu path. GPIO13 (pad 21) was rejected: channel B's own SWDIO/SWCLK
     traces box it in under the module edge, and a maze-router attempt produced a 20 mm detour.
4. **Hand-routed, locked tracks** (`tools/layout/reroute_rev_d.py`).
   - The TP102 +3V3 feed moved onto B.Cu to clear the drop corridors.
   - GPIO2 → R103 runs under D101 for the status LED.
   - A B.Cu ground jumper chain under R401–R403 was dropped; the freed B.Cu pour reconnects those stitching vias.
5. **Checks:** KiCad DRC with schematic parity gives 0 errors, 0 unconnected, 0 parity issues and no dangling copper.
   KiCad connectivity reports a single GND island.

## 10. USB-C 3D model
KiCad's 3D library has no model for the HRO TYPE-C-31-M-12 that the footprint references, so the connector rendered as
bare pads. J101 now uses LCSC/EasyEDA's STEP for C165948, stored in the project (`3dmodels/USB_C_HRO_TYPE-C-31-M-12_C165948.step`,
path `${KIPRJMOD}/...`). Offset (0, −1.02, 0) mm, rotation 180°.
- The offset was measured from renders: one known offset step showed the axis direction, and the body was then fitted
  to the footprint's F.Fab outline. Body front lands 0.05 mm from the board edge (KiCad outline: flush), and the shell
  sits on the board.
- Converting EasyEDA's own model origin gave an answer 1.5 mm off, so its metadata is not used directly.
- 3D only: Gerbers, BOM and CPL are unaffected. `32_3d_iso.png` shows the result.

## Reproducing
`tools/layout/` contains the scripts. They use KiCad's bundled Python (`kicad-…AppImage python3.11`) and contain absolute
paths from the session that produced them.
- `planner.py`, `layout8.py`: floorplan, overlap check and crossing score; writes the placement JSON (pushed with Konnect).
- `preroute8.py`: the locked power/USB/control skeleton.
- `route_all.sh` (`VIAC`, `BP`, `BA` environment variables): DSN export → Freerouting → session import → `maze.py` →
  `finish3.py` (pours and stitching) → `maze.py --gnd` → DRC → render.
- `maze_rr.py`, `beautify.py`, `cleanup.py`: final ground and stub clean-up.

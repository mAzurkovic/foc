# Layout check: foc_controller

Board: 45 x 45 mm, 6 layers, 1.6 mm, 2 oz outer / 1 oz inner, ENIG, epoxy-filled and capped vias (JLCPCB 6-layer, Standard assembly: parts on both sides plus through-hole). Order settings: `build/fab/STACKUP.md`.

## Status (2026-10-24, 6-layer, fully routed)
- All connections routed: DRC 0 errors, 0 unconnected, schematic parity 0, pcb-lint 0. Warnings: 11 silk_edge_clearance (connector outlines past the board edge), 1 silk_over_copper (D4's polarity circle on its own pad, clipped by the fab).
- ERC: 0 violations, 1 owner-approved exclusion (shared SPI: DRV SDO open drain + encoder MISO tri-state), see schematic-check.md.
- Stackup: F signal + power / In1 solid GND (no tracks) / In2 signal / In3 signal / In4 VBUS plane + 3.3 V island / B signal + power.
- Tracks 0.15 mm / 0.15 mm, signal vias 0.2 / 0.45 mm, power vias 0.3 / 0.6 mm. Via-in-pad on C35 (filled and capped).
- Current check (max flow over the copper, IPC-2152, 25 C rise): phases 25.8 A, pass-through VBUS 12.9 A, GND 35.8 A, VBUS feeds to Q1/Q3/Q5 11.6 to 11.9 A, all above 10 A. The 40 A / 2 s peak fails the adiabatic area on the VBUS feeds (0.32 of 0.59 mm2): accepted by the owner. 1 oz / 1 oz drops two VBUS feeds to 8.7 and 9.4 A; 1 oz / 0.5 oz to 5.8 to 6.5 A.
- Placement checks, accepted by the owner: C21 2.25 mm from U3.40 (limit 2), C22 3.36 mm from U3.38 (limit 2), C11 5.33 mm from U1.8 (limit 5; C12 at the pin passes), VBUS copper 8.6 mm from the encoder centre (limit 10).
- Changes in this pass: encoder on the DRV's SPI3 bus (three cross-board links gone), R25 DRV nSCS pull-up next to the MCU, J6 SWD pads moved to the board edge (motor side), USB pair skew rule 10 mm (full-speed USB), solid GND connection for the power-stage parts, one-spoke minimum for small GND pads, silkscreen designators placed clear (R25 on the fab layer only).
- Fab files: `build/fab/` (gerbers, BOM one line per LCSC part, CPL with the USB-C / JST GH +180 deg corrections). Renders: `build/render/foc_controller/`.
- Sections below are from the 4-layer layout (v13) and kept for history; the 4-layer stackup rows no longer apply.

## Board setup
| Item | Value | Source |
|---|---|---|
| Stackup | L1 signal + power, L2 solid GND, L3 power pours + signals, L4 GND + bottom parts | layout rules |
| Dielectric | 0.21 mm 7628 prepreg L1-L2 and L3-L4, 0.96 mm core | JLC 4-layer 1.6 mm |
| Fab limits (JLC capabilities page, 2026-10-21) | 2 oz outer: 0.15 / 0.15 mm track / space. 1 oz inner: 0.09 / 0.09 mm (held at 0.1). Via 0.2 mm hole / 0.45 mm: smallest with no extra charge | jlcpcb.com/capabilities |
| Default, SENSE, CAN | 0.15 mm track, 0.15 mm clearance (inner 0.1 / 0.1 by rule), via 0.2 / 0.45 mm | fab minimum; signal currents in mA |
| USB | 0.25 mm / 0.2 mm gap, about 90 ohm differential (coupled microstrip on 0.21 mm, er 4.4) | impedance |
| CAN pair | 0.15 mm / 0.4 mm gap, about 126 ohm (120 ohm +-20 %) | impedance |
| HV_SIG (charge pump, buck SW / BST) | 0.2 mm, via 0.2 / 0.45 mm. Buck SW peak 0.61 A (ILIM): 0.2 mm of 2 oz outer carries about 1.2 A at 10 C rise (IPC-2221 external); kept on outer layers (0.2 mm of 1 oz inner: about 0.4 A) | IPC-2221 |
| GATE_HS / GATE_LS | 0.3 mm (about 1.7 A on 2 oz outer for 10 C; gate peaks about 1 A for tens of ns), necked to the pad inside the DRV courtyard, via 0.3 / 0.6 mm | layout rules |
| PWR (+5V, +3V3, DVDD, VGLS) | 0.25 mm (about 1.45 A outer, 0.44 A inner 1 oz for 10 C; rails carry under 0.2 A), via 0.2 / 0.45 mm (0.5 A each) | IPC-2221 |
| HV (VBUS, phases), HC (shunt tops) | zones; tracks 0.3 mm floor (SHx gate returns paired with the 0.3 mm gate tracks, Kelvin and via links), 0.6 mm outer clearance to other nets, power vias 0.3 / 0.6 mm | rules file |
| Custom rules | hw/foc_controller.kicad_dru: HV clearance, outer 0.15 floor, inner 0.1, widths, signal and power vias, hole-to-track 0.2, sense-to-phase 1 mm, edge 0.5 mm, courtyard 0.2 mm | |

## Placement
- Connector edge (top). Top side: J1 XT30 IN and J2 XT30 OUT, flush with the edge, centres 23.44 mm apart. Motor side: J7 CAN IN, J9 USB-C, J8 CAN OUT, each JST-GH under its XT30. The USB-C 3D model was 180 deg reversed (pin tails at the mouth); rotated so the pins sit on the pads and the mouth is flush with the edge.
- M2.5 holes: bottom pair 2.8 mm from the side and bottom edges; top pair 2.8 mm from the side edges and 9.9 mm down, under the XT30 ear tabs.
- Power stage: Q1-Q6 in one row on top along the bottom edge, HS / LS alternating, HS drain pads 10.0 mm from the encoder centre. Motor wire pads J3-J5 below each pair. Motor side under each pair: 1206 shunt (pad 1 under the LS source, GND pad under the HS), its 4.7 uF and 100 nF, Kelvin net ties on the inner top corners of the shunt pads.
- DRV8353S on top directly over the encoder (owner's choice), rotation 0: control and SPI pins face the MCU, gate pins face the FET row. Its supply caps by hand: C20 (CPH/CPL) and C19 (VCP) along pins 1-5 with a lane between them for the CPH trace, C18 (VM) beyond C19, C22 (DVDD) over pin 38, C21 (VGLS) beside it, C23 (3V3) on the east side.
- MCU to the right of the DRV, rotation 90 (rotation search below). Its 100 nF caps on the motor side under their pins, bulk caps beside it, crystal below it.
- Buck on the left between J1's power pins and the gate-A path: L1 at the edge, U1 beside it, D1 under the L1-U1 junction, C14 (bootstrap) and C12 (input 100 nF) above U1 under J1, the rest below.
- 5 V path between the XT30s on top: LDO U2, diode-OR D2 / D3, C16 / C17, C47 (bulk at the connector edge). No CAN termination on the board (off-board at each chain end).
- Motor side: encoder at the centre (7.5 mm magnet ring kept free), CAN transceiver beside J8, USB ESD and CC resistors at J9, ADC filters and pull-ups under the MCU, SWD pads J6 beside the magnet ring, BOOT0 pad JP1 at the right edge.

## Rotation search (ratsnest crossings, DRV and MCU links to placed parts)
| DRV rot | MCU rot | All crossings | DRV-MCU bus crossings |
|---|---|---|---|
| 0 | 90 | 29 | 7 |
| 0 | 0 | 80 | 59 |
| 0 | 180 | 84 | 16 |
| 0 | 270 | 104 | 50 |
| 90 | 90 | 106 | 48 |
| 90 | 0 | 110 | 24 |
| 90 | 270 | 143 | 16 |
| 180 | 270 | 312 | 7 |
Chosen: DRV 0, MCU 90 (MCU right of the DRV; the left side holds the buck at the VBUS-in connector). Other 8 combinations: 148 to 376. Counted after the pin swaps; before them the same pick scored 49 / 14.

## Pin swaps (owner approved 2026-10-21; applied to the MCU sheet, ERC 0; every function checked in DS12288 Rev 5 Table 12)
The DRV SPI sits on SPI1 (PA5-7), on the MCU side away from the DRV, while the encoder's SPI3 pins face the DRV. Swapping the two ports and three GPIOs:

| Net | Old pin | New pin | Reason |
|---|---|---|---|
| DRV_SCLK | PA5 (SPI1) | PC10 (SPI3_SCK, FT) | MCU side facing the DRV |
| DRV_SDO | PA6 (SPI1) | PC11 (SPI3_MISO, FT_f) | same |
| DRV_SDI | PA7 (SPI1) | PB5 (SPI3_MOSI, FT_f) | same |
| ENC_SCK / ENC_MISO / ENC_MOSI | PC10 / PC11 / PB5 (SPI3) | PA5 / PA6 / PA7 (SPI1) | encoder is on the motor side anyway; its lines go down through vias |
| DRV_nSCS | PB7 | PA15 (FT_f, 5 V tolerant for the nSCS pull-up to DVDD) | top of the SPI group, matching the DRV pin order |
| CAN_TX | PA15 | PB4 (FDCAN3_TX, AF11) | frees PA15. PB4 is a UCPD dead-battery pin: TCAN3413 STB has an internal pull-up (standby) until firmware drives it low, so the bus is never driven before firmware disables dead-battery |
| DRV_nFAULT | PB10 | PB6 (FT_c) | under the SPI group, matching the DRV pin order |
| DRV_EN | PB11 | PB12 (TT_a, 3.3 V output is fine) | end of the MCU's PWM row, matching DRV pin 31 |
| ENC_CS | PB12 | PB11 | follows the encoder to the far side |

Each port stays separate, so USB-only encoder reads still never clock the unpowered DRV. Effect after F8: DRV-MCU bus 14 to 7 crossings; ratsnest crossings of all IC links on the board 166 to 125. The 7 left: 6 between the INHx and INLx groups (TIM1 puts CH1-3 and CH1N-3N in two groups, the DRV interleaves them per phase; INLx go on L3) and 1 between SDI and SDO (SPI3 has one MOSI pin here).

## Escape budget (0.2 / 0.45 mm vias, 0.6 x 0.55 mm pitch, ring 0.35-2.6 mm outside the pads, both sides free)
| IC side | Vias needed | Via sites | Needed x 1.2 <= sites |
|---|---|---|---|
| U3 N (INHx / INLx, EN) | 7 | 44 | pass |
| U3 S (gates B / C, sense B / C) | 10 | 76 | pass |
| U3 E (SOx, SPI, nFAULT) | 7 | 62 | pass |
| U3 W (gate A, sense A, VBUS) | 6 | 38 | pass |
| U4 N (PWM, DRV EN, USB, SWDIO) | 11 | 77 | pass |
| U4 S (crystal, ADC inputs) | 8 | 18 | pass |
| U4 E (encoder SPI, LEDs, encoder CS) | 10 | 19 | pass |
| U4 W (DRV SPI, CAN, SWCLK, BOOT0) | 10 | 69 | pass |
Counted after the pin swaps.

## Placement checks
| Check | Item | Measured | Limit | Result |
|---|---|---|---|---|
| Decoupling / supply cap pad to pin | C24 to U4.23 | 0.0 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C25 to U4.35 | 0.0 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C26 to U4.48 | 0.02 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C28 to U4.21 | 0.97 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C30 to U4.20 | 0.75 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C29 to U4.21 | 1.37 mm | <= 5 mm | pass |
| Decoupling / supply cap pad to pin | C31 to U4.20 | 1.79 mm | <= 5 mm | pass |
| Decoupling / supply cap pad to pin | C27 to U4.1 | 0.34 mm | <= 5 mm | pass |
| Decoupling / supply cap pad to pin | C23 to U3.24 | 0.34 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C18 to U3.3 | 4.02 mm | <= 2 mm | over, deviation 2 |
| Decoupling / supply cap pad to pin | C19 to U3.5 | 0.3 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C20 to U3.1 | 0.41 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C21 to U3.40 | 1.78 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C22 to U3.38 | 0.15 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C41 to U6.3 | 0.17 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C42 to U6.5 | 0.0 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C40 to U5.11 | 0.75 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C16 to U2.1 | 0.29 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C17 to U2.5 | 0.89 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C11 to U1.8 | 6.0 mm | <= 2 mm | over, deviation 3 |
| Decoupling / supply cap pad to pin | C12 to U1.8 | 1.2 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C13 to U1.7 | 1.11 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C14 to U1.2 | 1.85 mm | <= 2 mm | pass |
| Crystal to MCU pins | Y1 to U4.5/6 | 3.43 mm | <= 5 mm | pass |
| ESD to connector pins | U7 to J9 D+/D- | 3.21 mm | <= 5 mm | pass |
| CAN transceiver to connectors | U6 to J7 / J8 (CANH) | 32.67 / 9.23 mm | short stub | pass |
| Encoder keep-out (centre to nearest pad) | Phase copper (FETs, motor pads) | 13.85 mm | >= 10 mm | pass |
| Encoder keep-out (centre to nearest pad) | Bus copper (FETs, bulk caps, XT30) | 10.01 mm | >= 10 mm | pass |
| Encoder keep-out (centre to nearest pad) | Shunt current path | 10.29 mm | >= 10 mm | pass |
| Encoder keep-out (centre to nearest pad) | Buck switch node | 14.66 mm | >= 10 mm | pass |
| Sensitive part to switch nodes (any pad, incl. DRV SHx pins) | Y1 to DRV SHx pin U3.17 (FET phase pads 9.07 mm, buck SW 22.6 mm) | 6.98 mm | >= 10 mm | over, deviation 4 |
| Bus cap to HS drain pad | phase A: ('C1',) to Q1 | 0.0 mm | <= 3 mm | pass |
| Bus cap GND to shunt GND end | phase A: to R8.2 | 1.57 mm | <= 3 mm | pass |
| Gate path, straight line (routed is longer) | phase A: GH Q1 / GL Q2 | 18.41 / 11.83 mm | <= 20 mm | pass |
| Bus cap to HS drain pad | phase B: ('C5',) to Q3 | 0.0 mm | <= 3 mm | pass |
| Bus cap GND to shunt GND end | phase B: to R9.2 | 1.57 mm | <= 3 mm | pass |
| Gate path, straight line (routed is longer) | phase B: GH Q3 / GL Q4 | 12.73 / 7.39 mm | <= 20 mm | pass |
| Bus cap to HS drain pad | phase C: ('C9',) to Q5 | 0.0 mm | <= 3 mm | pass |
| Bus cap GND to shunt GND end | phase C: to R10.2 | 1.57 mm | <= 3 mm | pass |
| Gate path, straight line (routed is longer) | phase C: GH Q5 / GL Q6 | 16.45 / 13.55 mm | <= 20 mm | pass |
| Encoder under the DRV (owner's choice) | U5 to DRV SHx pins (top side, 1.6 mm board, In1 GND between) | 0.17 mm | accepted | pass |
| Sensitive part to switch nodes | U5 to BUCK_SW | 11.58 mm | >= 10 mm | pass |
| XT30 power-pin joints to motor-side SMD pads | closest: J2.1 to R23.2 (pad boxes; round pad is farther) | 2.95 mm | >= 3 mm | pass |
| BOOT0 jumper / pull-down to VBUS copper | JP1 (motor side) / R11 (top) | 10.23 / 8.76 mm | >= 5 mm | pass |
| Pads to board edge (connectors excepted) | closest: L1 | 0.54 mm | >= 0.5 mm | pass |
| Courtyard overlaps, pad clearances (DRC) | whole board | 0 | 0 | pass |

## Deviations (owner decision needed or given)
1. Encoder under the DRV8353S (owner's choice). The DRV's SHx pins are 0.17 mm in plane from the encoder's pads, on the other side of the board: they sense phase voltage and carry gate current only; L2 GND sits between. Phase and bus copper stay >= 10 mm away (13.85 / 10.01 mm).
2. C18 (DRV VM 100 nF) 4.0 mm from its VM pin: pins 1-5 (CPL, CPH, VBUS, VBUS, VCP) share 2 mm of the DRV's west side; the flying cap C20 (0.41 mm) and VCP cap C19 (0.30 mm) get the spots at the pins, C18 sits one cap further out. VM draws only the charge-pump current; the bus caps C1 / C5 / C9 (4.7 uF) are on the same VBUS plane.
3. Buck C11 (1 uF input bulk) 6.0 mm from VIN: the 100 nF input cap C12 sits at the pin (1.2 mm) and carries the fast edges; C11 is the low-frequency reservoir on the VBUS plane.
4. Crystal Y1 6.98 mm from the DRV's SHC pin (gate return, milliamps) and 9.07 mm from the nearest FET phase pad (limit 10 mm): the crystal must stay within 5 mm of the MCU's oscillator pins, which face the FET row at MCU rotation 90. Options: accept (guard ring of GND vias around Y1 and its traces, solid L2 under it), or rotate the MCU 180 (crystal to the top, +37 crossings).
5. C47 (bulk at J1) sits between the XT30s 12.6 mm from J1's VBUS pin; C48 is 5.3 mm from J2's. No free top-side spot nearer J1 (buck parts under J1). Both join the bus through the VBUS plane.

## Routing status (2026-10-21, handed to the owner for the last connections)
Done by script and locked: power-stage pours on top and bottom, via arrays (VBUS in the drain pads and strips, shunt
nodes through the LS source pins, GND between cells), L2 solid GND, L3 VBUS ring and band, bulk-cap VBUS vias and
bars, one GND via per small-part GND pad, exposed-pad thermal vias. Freerouting routed about 85 % of the signals
(unlocked, edit freely). Board: `hw/foc_controller.kicad_pcb`.

First: open the board, run Update PCB from Schematic (F8) for the PWM channel swap (6 pad-net changes on U4), save.

### Rules to keep while routing (DRC enforces them)
- Signal tracks 0.15 mm, gates 0.3 mm, logic supplies 0.25 mm; signal vias 0.2 / 0.45 mm, power and gate vias 0.3 / 0.6 mm.
- 0.6 mm from exposed high-voltage pads (VBUS, phase, gate-high, charge pump, buck switch); tracks under mask need only the class clearance.
- Kelvin sense pairs and SOx lines 1.0 mm from VBUS / phase / switch-node copper, outside the DRV and shunt courtyards.
- No tracks on L2 (solid GND). No L3 tracks in the connector-edge band (y < 119.0 mm, the 10 A pass-through); vias are fine there.
- Locked copper is the power stage: leave it.

### Connections to route (49)
| Area | Nets | Notes |
|---|---|---|
| PWM bus (after F8) | INHA, INHB, INHC, INLA, INLB, INLC, DRV_EN | DRV north pins 31-37 to MCU north pins 25-32. INHx on top as a nested U (DRV pins north, over, down into U4 pins 30-32); INLx and DRV_EN on the bottom, via north of the DRV pins (y about 120.8, between the INH traces) and north of U4 pins 26-29 (y about 118.2 / 118.9, staggered). Delete the stray INHB via at (124.1, 120.0). |
| Gate drive | GHC, PHASE_A/B/C (DRV SHx pins 7 / 14 / 17), DRV_CPH | Each SHx runs beside its gate trace to the HS source / phase node. DRV_CPH: leave pin 2 straight west, then to C20 pad 1. Delete the stray GLB via at (126.0, 133.7). |
| MCU periphery | XTAL_IN, XTAL_OUT, NRST, LED_G, ENC_SCK, ENC_MOSI, SOA, SOA_ADC, SOB_ADC, BOOT0 (U4.46 - R11 - JP1) | Crystal: short, on top, GND under it. Delete stray vias ENC_MOSI (129.8, 127.9), LED_G (141.3, 128.9), SWCLK (122.1, 120.6). |
| CAN and USB | CAN_TX, CAN_RX, CAN_STB, CAN_L, USB_DP, USB_DM, USB_CC1, USB_CC2 | USB pair: keep D+ / D- together, skew under 0.5 mm. |
| Power links | VBUS (U1 VIN, R1, C11, C18, C19, DRV pins 3 / 4, C2, C8, VBUS_top_A to _B, VBUS_top_C to the right bar), +3V3 (U2.5, U6.3, C24 area), +5V (C16), NTC_SENSE | VBUS tracks 0.5 mm where they carry the buck input. |

### DRC items left from the autorouter (26 clearance)
- Sense spacing: C_SENSE_P / C_SENSE_N on L3 near VBUS vias (132-138, 131-139), B_SENSE_P/N and A_SENSE_P vias near the Q3 drain and DRV pin 14, SOB_ADC near VBUS vias at (133.6, 131.5), SOC via near C48: move them 1 mm away.
- C_SENSE_P on the bottom touches NT6's net-tie copper at (135.1-135.6, 134.4): reroute from NT5.
- High-voltage pad spacing: +5V_USB via 0.55 mm from C20 pad 1; LED_R and NTC_SENSE vias near C8 pad 1; NRST via near C49 pad 1.
- Thermal relief warnings (12) are pour spokes: handled in the finishing pass (pour settings).

## Not yet checked (after routing)
Hot-loop area, via arrays at each top-to-bottom power transition, phase copper area per phase, pour necks, sense-pair spacing, gate pair routing, GND plane continuity, EMC and thermal analyzers, silkscreen.

# Layout check: foc_controller

Board: 45 x 45 mm, 4 layers, 1.6 mm, 2 oz outer / 1 oz inner, ENIG (JLCPCB 4-layer, Standard assembly: parts on both sides plus through-hole).
Generated from pad coordinates in `hw/foc_controller.kicad_pcb` (script, not eyeballed).

## Status
- Stage: placement approved by the owner (2026-10-20). Nothing routed.
- ERC 0. DRC: only silkscreen items (overlaps, text over copper, text at edge), fixed in the silkscreen pass after routing. 285 unrouted connections (expected before routing).
- Parts: 57 top, 61 motor side. Silkscreen reference labels shown only for ICs, connectors, FETs, switch, jumper, LED and test points; passives are identified in the BOM / placement files.

## Board setup
| Item | Value | Source |
|---|---|---|
| Stackup | L1 signal + power, L2 solid GND, L3 power pours + signals, L4 GND + bottom parts | layout rules |
| Dielectric | 0.21 mm 7628 prepreg L1-L2 and L3-L4, 0.96 mm core | JLC 4-layer 1.6 mm |
| USB pair | 0.25 mm / 0.2 mm gap, ~90 ohm | coupled microstrip estimate on this stackup |
| CAN pair | 0.2 mm / 0.6 mm gap, ~119 ohm | same |
| Netclasses | HV (VBUS, phases: 1.0 mm min track, 0.6 mm outer clearance), HV_SIG (buck SW/BST, charge pump: 0.6 mm outer clearance), HC (shunt tops), GATE_HS / GATE_LS 0.4 mm, SENSE, PWR 0.4 mm, USB, CAN | hw/foc_controller.kicad_pro |
| Custom rules | hw/foc_controller.kicad_dru (HV clearance, widths, vias, sense-to-phase 1 mm, edge 0.5 mm, courtyard 0.2 mm) | |

## Placement
- Board: 45 x 45 mm, 2 mm corner radius. M3 holes in all four corners, 2.8 mm in from each edge (39.4 mm square).
- Connector edge (top edge). Top side: J1 XT30 IN and J2 XT30 OUT, centres 19.2 mm apart, mating face 0.8 mm inside the edge so the front pegs clear the USB-C pads below. Motor side: J7 CAN IN, J9 USB-C, J8 CAN OUT, each JST-GH under its XT30.
- Power stage: Q1-Q6 in one row on top along the power edge, HS / LS alternating, row top 9.15 mm below the encoder centre (HS drain pads 10.0 mm from it). Motor wire pads J3-J5 below each pair, between the bottom holes. Motor side, directly under each pair: shunt (R8 / R9 / R10, shunt pad under the LS source, GND pad under the HS), its 4.7 uF (C1 / C5 / C9) and 100 nF (C50-C52), and the Kelvin net ties at the shunt pad ends.
- Bulk caps: C47 / C48 on top right behind the XT30 power pins; 11 more on the motor side in left and right groups beside the FET row. The outer columns (C2-C4, C8, C10) lie parallel to the side edges, 1.28 mm in.
- Logic, top: LDO U2 with C16 / C17 between the XT30s, MCU left, BOOT0 pull-down R11 at the left edge, DRV8353S centred over the FET row, buck on the right (L1 at the edge), termination switch at the left edge (clear of a 7 mm washer on H1), NTC above Q3, GND test pad TP4 beside Q1.
- Logic, motor side: encoder at the centre (silkscreen ring, no other parts within 7.5 mm of its centre), CAN transceiver beside J8, USB ESD and CC resistors at J9, diode-OR D2 / D3 and USB VBUS cap C43 where space allowed, ADC filters and pull-ups right, SWD pads J6 along the right edge, BOOT0 pad JP1 at the left edge below H1.
- Keep-outs on the motor side: 3 mm around each XT30 power-pin joint (hand-soldered after reflow), 5 mm from VBUS copper for the BOOT0 pad.
- Fabrication: order with "Edge rails/fiducials: added by JLCPCB", mouse-bite joints requested in the order remark (no V-cut). No fiducials on the board itself.

## Placement checks
| Check | Item | Measured | Limit | Result |
|---|---|---|---|---|
| Decoupling / supply cap pad to pin | C24 to U4.23 | 0.73 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C25 to U4.35 | 0.73 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C26 to U4.48 | 1.46 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C28 to U4.21 | 3.26 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C30 to U4.20 | 0.79 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C29 to U4.21 | 7.93 mm | <= 5 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C31 to U4.20 | 9.73 mm | <= 5 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C27 to U4.1 | 2.43 mm | <= 5 mm | pass |
| Decoupling / supply cap pad to pin | C23 to U3.24 | 0.61 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C18 to U3.3 | 5.02 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C19 to U3.5 | 4.63 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C20 to U3.1 | 1.92 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C21 to U3.40 | 5.47 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C22 to U3.38 | 3.6 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C41 to U6.3 | 0.76 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C42 to U6.5 | 3.57 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C40 to U5.11 | 0.76 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C16 to U2.1 | 3.09 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C17 to U2.5 | 3.09 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C11 to U1.8 | 3.83 mm | <= 2 mm | over, see deviations |
| Decoupling / supply cap pad to pin | C12 to U1.8 | 1.51 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C13 to U1.7 | 1.06 mm | <= 2 mm | pass |
| Decoupling / supply cap pad to pin | C14 to U1.2 | 3.36 mm | <= 2 mm | over, see deviations |
| Crystal to MCU pins | Y1 to U4.5/6 | 3.56 mm | <= 5 mm | pass |
| ESD to connector pins | U7 to J9 D+/D- | 3.18 mm | <= 5 mm | pass |
| CAN transceiver to connectors | U6 to J7 / J8 (CANH) | 17.0 / 2.33 mm | short stub | pass |
| Encoder keep-out (centre to nearest pad) | Phase copper (FETs, motor pads) | 13.85 mm | >= 10 mm | pass |
| Encoder keep-out (centre to nearest pad) | Bus copper (FETs, bulk caps, XT30) | 10.01 mm | >= 10 mm | pass |
| Encoder keep-out (centre to nearest pad) | Shunt current path | 10.29 mm | >= 10 mm | pass |
| Encoder keep-out (centre to nearest pad) | Buck switch node | 11.98 mm | >= 10 mm | pass |
| Sensitive part to switch nodes (any pad, incl. DRV SHx pins) | Y1 to PHASE_x / BUCK_SW | 13.24 mm | >= 10 mm | pass |
| Sensitive part to switch nodes (any pad, incl. DRV SHx pins) | U5 to PHASE_x / BUCK_SW | 2.38 mm | >= 10 mm | over, see deviations |
| Bus cap to HS drain pad | phase A: C1 + C50 to Q1 | 0.62 mm | <= 3 mm | pass |
| Bus cap GND to shunt GND end | phase A: to R8.2 | 0.85 mm | <= 3 mm | pass |
| Gate path, straight line (routed is longer) | phase A: GH Q1 / GL Q2 | 15.89 / 9.91 mm | <= 20 mm | pass |
| Bus cap to HS drain pad | phase B: C5 + C51 to Q3 | 0.62 mm | <= 3 mm | pass |
| Bus cap GND to shunt GND end | phase B: to R9.2 | 0.85 mm | <= 3 mm | pass |
| Gate path, straight line (routed is longer) | phase B: GH Q3 / GL Q4 | 9.26 / 3.89 mm | <= 20 mm | pass |
| Bus cap to HS drain pad | phase C: C9 + C52 to Q5 | 0.62 mm | <= 3 mm | pass |
| Bus cap GND to shunt GND end | phase C: to R10.2 | 0.85 mm | <= 3 mm | pass |
| Gate path, straight line (routed is longer) | phase C: GH Q5 / GL Q6 | 13.51 / 11.6 mm | <= 20 mm | pass |
| XT30 power-pin joints to motor-side SMD pads | closest: J1.2 to R17.2 | 3.69 mm | >= 3 mm | pass |
| BOOT0 jumper / pull-down to VBUS copper | JP1 (motor side) / R11 (top) | 5.74 / 5.76 mm | >= 5 mm | pass |
| Edge 1210 bus caps along the edge | C2, C3, C4, C8, C10 rotated 90 deg | 1.28 mm from edge | >= 1 mm | pass |
| C15 to board edge | C15 | 1.22 mm | >= 1 mm | pass |
| SW1 pad to H1 centre (7 mm washer = 3.5 mm) | SW1 | 4.39 mm | >= 3.5 mm | pass |

## Deviations (owner decision)
1. Encoder clearance 10 mm, not the earlier 12 mm (bus copper 10.0 mm, shunts 10.3 mm, XT30 pins 10.4 mm, phase copper 13.9 mm). The 45 mm board leaves no room for more. AS5047P magnet field 35 to 70 mT, uniform stray fields rejected; 40 A at 10 mm is about 0.8 mT for a lone conductor, less with the return on the adjacent layer.
2. Encoder (motor side) is 2.4 mm in plane from the DRV8353S SHx pins (top), the DRV sits over it. Those pins sense voltage and carry milliamps; the L2 GND plane sits between them and the encoder.
3. DRV8353S caps C18 (VM 100 nF, 5.0 mm), C19 (VCP, 4.6 mm), C21 (VGLS, 5.5 mm), C22 (DVDD, 3.6 mm) over 2 mm: the area under the DRV is the magnet area. Target: under about 3 mm by rotating them during routing.
4. MCU bulk caps C29 / C31 at 7.9 / 9.7 mm (limit 5 mm) after the MCU moved down for C47; C28 at 3.3 mm. On the 3.3 V plane; pulled in during routing.
5. Buck C11 (input 1 uF, 3.8 mm) and C14 (bootstrap, 3.4 mm); LDO C16 / C17 (3.1 mm); CAN C42 (3.6 mm). The 100 nF input cap C12 carries the fast edges of the buck.
6. Mounting holes 2.8 mm from the edges: the bottom pair's courtyards clear the end FETs, an M3 head (5.5 mm) stays 0.05 mm inside the edge. A washer overhangs.
7. Diode-OR D2 / D3 sit 9-13 mm from the LDO and C43 (USB VBUS 1 uF) 12.7 mm from the USB-C: the motor-side space near the connector edge is taken by the 3 mm XT30 keep-outs. All on the 5 V path, DC loads; no fast edges.

## Not yet checked (after routing)
Hot-loop area, via arrays at each top-to-bottom power transition, phase copper area per phase, pour necks, sense-pair spacing, gate pair routing, GND plane continuity, EMC and thermal analyzers, silkscreen.

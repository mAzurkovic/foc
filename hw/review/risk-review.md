# Risk review: foc_controller (schematic + placement)

Date: 2026-10-20. Board state: 45 x 45 mm placement v9, not routed. Schematic as committed in 9a13c5c plus the uncommitted cap and test-pad changes.

How this was done: a walk through every power path, fault path and mechanical interface, plus the kicad-happy schematic, PCB, cross-check, EMC and thermal analyzers. Distances are measured from pad coordinates in the board file.

Severity: **High** = can destroy the board, the chain or the PC, or causes a respin. **Medium** = degrades performance or makes assembly or repair harder. **Low** = worth knowing.

## A. Fix in placement before routing

| # | Sev | Finding | Failure mode | Proposed fix |
|---|---|---|---|---|
| A1 | High | JP1 (BOOT0 solder jumper, 3.3 V side) is 1.4 mm from the J1 VBUS through-hole pin on the motor side. R11 is 2.3 mm from it. | Bridging JP1 with an iron for DFU recovery puts a solder blob next to 48 V. If it touches, 48 V lands on the 3.3 V rail and kills the MCU, encoder, CAN transceiver and LDO. | **Done.** R11 on top at the left edge, 5.76 mm from VBUS copper; JP1 on the motor side at the left edge below H1, 5.74 mm. |
| A2 | Medium | The design rule "motor-side SMD ≥ 3 mm from the XT30 solder joints" is broken: D3 is 0.9 mm from the J2 GND pin, R20 1.6 mm and U2 1.8 mm from J2, C16 1.9 mm, JP1/R11 from J1 (A1). | JLC hand-solders through-hole parts after reflow, from the motor side. A large iron on a 2 oz power pin with plane connections sits for seconds and can reflow or bridge the small parts next to it. | **Done.** 3 mm keep-out around each power pin on the motor side; closest SMD pad now 3.69 mm (R17). The LDO U2 with C16 / C17 moved to the top between the XT30s; the diode-OR and C43 moved where space allowed (9-13 mm from the LDO). The USB-C pads near the ear pegs: see A3. |
| A3 | Low | The XT30 ear pegs have plated 1.8 mm pads (1.0 mm drill), 0.8 mm from the USB-C pins. The Amass drawing shows them as 1.0 mm locating holes, and the pegs look like plastic. | None if nobody solders them. If they are metal and someone solders them, the joint sits 0.8 mm from the USB-C signal pins. | **Resolved (owner checked, 2026-10-20): the pegs are metal**, so the plated pads stay and get soldered. The pads stay unconnected on purpose: a bridge from an ear to one USB-C pin then does nothing; only a blob spanning two USB-C pins would short (USB 5 V to GND, which the PC port's current limit handles). The ears are small and plane-free, so they solder in a second, unlike the power pins. Assembly note: inspect the USB-C pins after the XT30 is soldered. |
| A4 | High | No fiducials on either side. Parts are 0.40-0.47 mm from the board edge (L1, J6, C15, SW1), and the 1210 bus caps C2, C3, C4, C8, C10 are 0.7 mm from the side edges. | Without rails, JLC can't fixture a 45 mm board with parts at the edge, and pick-and-place on 0.4-0.5 mm pitch QFNs needs fiducials. With V-cut rails, breaking the rail flexes the board right where those 1210s sit. A cracked bus MLCC fails short across 48 V, and there is no fuse (see B4). | **Done on the board:** the five edge 1210s now lie along the edge, 1.28 mm in; C15 1.22 mm in. **At ordering:** JLC edge rails joined by routed tabs (mouse bites), not V-cut, with fiducials on the rails. Written into the design doc. |
| A5 | Low | SW1 (termination switch) pad is 3.4 mm from the H1 hole centre. An M3 head is 5.5 mm (2.75 mm radius), a standard M3 washer 7 mm. | A washer under the screw head on the top side covers the switch. | **Done.** SW1 moved 1 mm down: pad edge 4.39 mm from the H1 centre, clear of a standard 7 mm M3 washer. |

## B. Electrical risks (schematic and system)

| # | Sev | Finding | Failure mode | Mitigation / decision |
|---|---|---|---|---|
| B1 | High | The 10 A RMS no-heatsink rating at 48 V is not backed by data. The closest moteus board (n1, 46 x 46 mm) is rated 4.2 A at 48 V and 9.4 A at 24 V with no cooling, at a 60 °C limit. Scaling to our 90 °C limit gives about 5.7 A at 48 V and 12.7 A at 24 V. | The board hits the 90 °C derating early at 48 V, so the joint delivers less torque than its rating says. Not damaging: the firmware derates and trips. | Expect about 6 A continuous at 48 V and 10 A or more at 24 V. The thermal acceptance test on the first board sets the published number. Already written into the design doc. |
| B2 | High | Hot-plugging or unplugging a board on a live chain. 32 µF of ceramics with almost no damping plus the cable inductance ring up to about 2x the bus voltage. | At 48 V, a spike toward 90 V exceeds the 80 V rating of the FETs and the DRV8353S, killing the board's power stage. A TVS can't help: a 48 V TVS clamps around 78-90 V, so it doesn't clamp below 80 V. | Process only: connect or disconnect with the chain off, power through the XT90 anti-spark. Put it in the user docs as damaging. At a 24 V bus there is enough margin. |
| B3 | High | Any fault turns all six FETs off and the motor coasts. A spinning motor whose back-EMF exceeds the bus rectifies through the FET body diodes into the bus. | With a power supply (which can't absorb energy) and no regen clamp, the bus climbs past 80 V and kills every board on the chain. The firmware's regen limit can't act, because its outputs are off. | The regen clamp module is mandatory with a power supply, not optional. Firmware design: on faults at high speed, consider an active short (all three low-side FETs on) instead of coasting, when the fault allows it. Record this in design-firmware.md. |
| B4 | High | No fuse anywhere, by design choice. Failures that short the bus: a cracked MLCC (16 of them across the bus), a shorted FET pair, a reversed hand-made male-to-male lead (current flows through all six body diodes). | A LiPo or a big supply dumps hundreds of amps into the short. The board burns, the XT30 or the wiring can catch fire, and the whole chain goes down with it. | One fuse or breaker at the supply, sized to the chain budget (15-20 A). It's off-board, so the board stays minimal. Use bought, pre-made leads, or check each hand-made lead with a meter before use. |
| B5 | Medium | The MCU (STM32G474CEU6, "6" = 85 °C ambient) and the JST-GH (85 °C) share a 45 mm board with FETs that trip at 110 °C. A small board has a small temperature gradient. | The MCU runs above its rating before the power-stage trip, so behaviour becomes undefined (flash, clock). | **Decided (owner, 2026-10-20): keep the 85 °C part**, with the firmware limit on the MCU's internal sensor. Considered: or switch to the 125 °C grade STM32G474CEU3 (same UFQFPN-48 package; JLC C5270382, 0 in stock on 2026-10-20, so it would be a consigned or pre-ordered part). The first-board thermal test should log the MCU temperature too. |
| B6 | Medium | Each overcurrent comparator trips in one current direction. | A large negative phase current is caught only by the DRV8353S VDS overcurrent latch, about 0.3 V across a 2.6 mΩ FET (about 115 A), much later than the configured trip. | Known open item. Firmware also checks the ADC phase currents every PWM cycle (both directions). Document the behaviour. |
| B7 | Medium | If a board's XT30 ground opens while its CAN cable is connected, the board's return current flows through the 100 Ω R20 into the CAN GND. | R20 (0603) sees up to 48 V, about 23 W, burns open and may scorch the board. It acts as a fuse. The TCAN3413 is rated ±58 V, so it survives the ground shift. | Accept: this is the intended sacrificial element. The board is unpowered after it (its ground is lost anyway). |
| B8 | Medium | USB connects the PC ground to the motor ground unless the isolator is used. | Motor return current finds a path through the USB cable and the PC: USB disconnects or a dead PC port. With the PC connected to two boards, a ground loop. | Isolator mandatory (already the documented setup). Put it on the label or in the quick-start guide. |
| B9 | Low | On MCU reset or hang, the DRV8353S inputs fall to their internal pull-downs, so the outputs turn off (safe). The independent watchdog is set by option byte. | Safe state, but it means coasting (see B3). | None beyond B3. |
| B10 | Low | Encoder: magnet face 0.5-2 mm from the AS5047P face. The IC is about 1.2 mm tall, so the magnet ends about 1.7-3.2 mm from the board's motor side. Shaft centring comes only from the four corner holes. | Off-centre magnet = angle error (harmonic ripple in torque and position). Too far = weak field, encoder fault. | The actuator design holds the magnet gap and centring (shaft-to-hole-pattern tolerance about ±0.3 mm). Setup measures the encoder offset; firmware can add an error-compensation table later if ripple shows. |
| B11 | Low | Bus ripple about 2.7 V p-p at 10 A with ceramic bulk (2 µF each at 48 V after DC bias). | Visible in VBUS readings and in phase-current noise. | The firmware samples VBUS every PWM cycle (in the design). Chain ringing remains a bench check (existing open item). |

## C. Routing must-dos that come from this placement

| # | Item | Why |
|---|---|---|
| C1 | Run the 15 A pass-through (J1 to J2, VBUS and GND) **between the XT30 pins and the connector edge**, on stacked layers. Not on the inner side of the pins. | A straight line between the pins passes 7.6 mm from the encoder centre; the edge side is 12 mm or more. |
| C2 | Via arrays at every top-to-bottom power transition: HS drain to bus cap, LS source to shunt, shunt and cap GND. About 10 or more 0.3 mm vias each. | The switching loop and up to 40 A pass through the board under each FET pair. Too few vias = heat and loop inductance. |
| C3 | Thermal vias under every FET pad, the DRV8353S, the MCU and the TCAN3413 pads. | Analyzer flagged none yet (expected before routing). With no heatsink, spreading heat into the inner planes is the cooling. |
| C4 | Shunt Kelvin pairs (on the motor side) leave from the inner pad edge, go up through vias as a pair, and stay away from the phase pours. | The current sense accuracy depends on it. DRC can't check it. |
| C5 | Encoder SPI from the MCU (top left) to the encoder (motor side, centre) over the solid GND plane, away from phase copper. DRV SHx sense traces lead away from the encoder. | 0.9 V/ns phase edges couple into nearby traces. |
| C6 | Motor wire pads: pour the phase copper solidly onto them, but expect a high-power iron (≥ 80 W) for 14-16 AWG wire on 2 oz, 4 layers. | A cold joint gives an intermittent phase: overcurrent trips or erratic motion. |
| C7 | Pull the DRV8353S caps (C18, C19, C21, C22), the buck caps (C11, C14), C16/C17 and the MCU bulk caps (C29, C31) back toward their pins. | Placement deviations 3-6 in layout-check.md. |

## D. Serviceability (accepted by design, worth knowing)
- SWD pads, the BOOT0 jumper and the USB-C are on the motor side: none are reachable with the board mounted. Firmware updates and recovery normally go over USB-C (dual-bank flash). A board that won't enumerate has to come off the motor.
- The USB-C needs an opening in the actuator housing at the connector edge, with room for a plug overmold (about 12 x 6.5 mm), inside the ≥ 5 mm clearance strip.

## E. Analyzer findings judged false or expected
| Finding | Verdict |
|---|---|
| LED D4 has no current-limit resistor | False: R12 / R13 sit in series with each colour. |
| USB_DP / USB_DM 5 V to 3.3 V domain crossing | False: USB data lines are 3.3 V signalling; only VBUS is 5 V. |
| +5V_USB, BUCK_VCC, VBUS_ADC have no declared source | False: they come from the USB connector, the LM5008A's internal regulator, and the divider. |
| Unrouted nets, no ground plane, no thermal vias, stitching, test-point coverage | Expected before routing. Rechecked after routing. |
| J1 / H1 courtyard overlap, 0.0 mm² | Touching, not overlapping. DRC is clean. |
| U1 (buck) harmonics in 30-88 MHz | Expected for a buck. Mitigated by the hot loop (C12 at VIN) and the GND plane under it in routing. |

## Decisions for the owner
1. A1, A2, A4, A5 placement fixes: done (2026-10-20).
2. A3: resolved, the pegs are metal; pads stay plated and unconnected.
3. B5: decided, keep the 85 °C MCU with the firmware limit.
4. B3, B4: done. The fuse (B8, every chain) and the regen clamp (B7, with a power supply) are required items in the design doc's off-board parts list, with a use-notes section for the user guide (power off before connecting, fuse rating, pre-made leads, isolated PC adapters).

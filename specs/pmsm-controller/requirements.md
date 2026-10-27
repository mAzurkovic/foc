# Low-cost PMSM Controller

Custom FOC controller board plus firmware for generic hobby PMSMs. Target use: daisy-chained robotic arm joints. `nucleo_foc/` is the proof of concept, not the product.

## User Stories
- As an arm builder, I want to set up each actuator once on the bench, so assembled joints run without re-tuning.
- As an arm builder, I want to chain up to 8 controllers on one power and comms cable, so each joint needs no harness back to a main board.
- As a host client developer (any language), I want one protocol document, so I can write clients that command joints without reading firmware source.
- As an arm builder, I want a CLI tool, so I can set up, tune, and command one board or a whole chain from a PC or Mac.

## Functional Requirements

### Supported motors
- FR1. Controller drives any 3-phase PMSM inside this envelope: pole pairs 1 to 30, phase inductance in µH ≥ bus voltage / 2.4 (5 µH at 12 V, 10 µH at 24 V, 20 µH at 48 V), phase resistance 0.02 to 5 Ω, kV 20 to 1000, within the board's voltage and current ratings.
- FR2. User supplies: pole pairs, kV, max voltage, rated current, reducer ratio (1 if none).

### Power stage
- FR3. Bus input 12 to 48 V DC nominal (fixed supply, or battery up to 12S, 50.4 V full).
- FR4. Phase current at 25 °C ambient, 48 V bus, board ≤ 90 °C: 10 A RMS continuous with no heatsink in still air, 40 A peak for 2 s. No heatsink or thermal pad option. Parts on both sides: the motor-facing side carries the encoder IC at the center (with a part-free area around it for the magnet), the USB-C and the two CAN connectors along the connector edge, the shunts and capacitors under the power stage, and small logic parts. The actuator leaves ≥ 3 mm of clearance under the motor-facing side and ≥ 5 mm under the connector-edge strip.
- FR5. Above 25 °C ambient, continuous rating set by board overtemperature derating (FR28).
- FR6. Power stage disabled whenever bus voltage is below the configured min (FR27). Overvoltage handled per FR26.

### Position sensing and reference
- FR7. On-board on-axis magnetic encoder, 14-bit or better, reads a diametric magnet on the motor shaft. No external encoder input. Sensorless and hall-only operation not supported.
- FR8. Reducer ratio = 1: position absolute at power-up.
- FR9. Reducer ratio > 1: position flagged "unreferenced" at power-up. Position mode refused until referenced. Speed and torque modes available immediately.
- FR10. Set-zero command: host sends an output-shaft angle in degrees (e.g. 0 or 90); the controller records the present position as that angle and marks the node referenced.
- FR11. Home command: rotate in configured direction at configured speed with configured current limit until stall at a hard stop; set position to configured offset; mark referenced. Report "homing failed" if no stall within configured max travel.

### Configuration mode
- FR12. Setup command measures phase R, phase L, encoder electrical offset and direction, total inertia at the motor shaft, and the cogging table. It then sets current-loop gains and default speed and position gains.
- FR13. Setup assumes the actuator rotates freely (run per actuator, before assembly). Setup current ≤ 50% of rated current.
- FR14. Setup aborts with outputs off and a named error when: a measured value contradicts a supplied one (e.g. measured pole pairs ≠ supplied), or the rotor stalls.
- FR15. Manual tuning command reads and writes speed-loop and position-loop gains, limits, motion profile parameters, timeout, homing parameters, regen limit, node ID.
- FR16. Explicit save command writes all configuration and setup results to non-volatile memory. Nothing is persisted without save.
- FR17. At boot, saved configuration is restored. On checksum mismatch: boot into Configuration mode, outputs disabled, report "config invalid".

### Operation mode
- FR18. Idle (motor off): outputs disabled, motor coasts, no fault raised. Default mode at power-up and after setup. Host leaves Idle by selecting another mode.
- FR19. Torque mode: setpoint in output-shaft N·m. Kt = 8.27 / kV × reducer ratio. Phase-current accuracy ±5% of reading ±0.3 A, with current-sense offset calibrated automatically each time outputs are enabled.
- FR20. Speed mode: setpoint in output-shaft rpm. Supports motor electrical frequency up to 1.5 kHz. Steady-state error ≤ max(1% of setpoint, 2 rpm).
- FR21. Position mode: setpoint in output-shaft degrees. Unloaded steady-state error ≤ 2 encoder counts at the motor, averaged over 100 ms. Zero overshoot with default gains after setup.
- FR22. Position profile selectable per node: `trapezoidal` (default; configurable max speed, acceleration, deceleration) or `direct` (setpoint goes straight to position loop, bounded only by speed and current limits).
- FR23. Controller starts acting on a new setpoint within 1 ms of receipt (latency, not completion). Time to reach the setpoint depends on the motion profile, limits, and load.

### Faults
Every fault below disables outputs (motor coasts), latches, and blocks motion commands until the host clears it, unless stated otherwise.
- FR24. Phase overcurrent: trip level configurable ≤ 45 A. Response within 1 PWM period.
- FR25. Encoder fault: invalid readings for > 1 ms.
- FR26. Bus overvoltage: above configured max (setting ≤ 52 V) the controller stops returning power and burns braking energy in the motor windings, outputs stay on, no fault. Above configured max + 2 V (≤ 54 V): outputs disabled, fault latched.
- FR27. Bus undervoltage: below configured min, min setting ≥ 10 V.
- FR28. Board overtemperature: current derated linearly from 90 °C, trip at 110 °C, measured at the power stage.
- FR29. Gate driver fault reported by the driver.
- FR30. Position following error: |setpoint − actual| above configured limit for longer than configured time.
- FR31. Duplicate node ID, config invalid, KILL received: latched, motion refused.
- FR32. Comms timeout: no valid host command within configured timeout (10 to 1000 ms, default 50 ms). Response: hold present position (outputs stay on) and latch "comms timeout". Timeout = 0 (disabled) allowed over USB-C only.

### Regenerative braking
- FR33. Power returned to the bus limited to a configured value, default 0 W. Excess braking energy dissipated in the motor windings. No on-board brake resistor; a chain run from a non-absorbing power supply uses one external regen clamp / brake-resistor module at the supply.

### Daisy chain
- FR34. Each board has power-in, power-out, bus-in, bus-out connectors. Power and bus pass straight through the board; a failed or unpowered node never cuts off downstream nodes.
- FR35. Power pass-through rated 10 A DC continuous per board (480 W at 48 V). User keeps summed downstream average bus draw ≤ 10 A; larger arms split into multiple chains.
- FR36. Chain supports 8 nodes over 2 m total cable.
- FR37. Host exchanges with every node at 1 kHz: setpoint out; position, speed, phase current, bus voltage, bus current, board temperature, fault status back.
- FR38. No CAN termination on the board. The chain is terminated off-board with a split terminator (about 120 Ω) at each end: in the host adapter lead at the first board, and in a plug-in terminator in the last board's bus-out connector.
- FR39. A node that stops responding is detectable by the host within 10 ms (10 missed cyclic replies).
- FR40. No on-board inrush limiting. Chain powered through one external anti-spark or precharge switch at the supply; boards connected and disconnected only with the chain unpowered. Reverse polarity prevented by keyed connectors, not circuitry.

### Addressing
- FR41. Node ID 1 to 8, assigned over USB-C or bus, saved with config.
- FR42. New boards ship with ID 0 ("unassigned"); ID 0 ignores motion commands.
- FR43. One discovery command returns every node's ID.
- FR44. Two nodes with the same ID both raise duplicate-ID fault and refuse motion.

### KILL
- FR45. Broadcast KILL: every node disables its power stage within 2 ms, latches "killed", refuses motion until host clears that node. Logic and bus stay up; nodes keep reporting status.
- FR46. KILL also available over USB-C for a single node.

### USB-C
- FR47. USB-C supports Configuration mode and Operation mode for one board. No chain access through USB-C.
- FR48. USB-C alone powers logic: config read/write, firmware update, ID assignment, encoder readout work. Motion refused with "no bus power".
- FR49. DC bus never back-feeds USB; USB never feeds the DC bus. The PC never shares a ground with motor power: it connects to a board only through an off-the-shelf galvanically isolated USB adapter, and to the chain only through an isolated USB-to-CAN FD adapter. The board itself is not isolated.

### Firmware update
- FR50. Firmware update over USB-C (single board) and over the bus by node ID.
- FR51. Update interrupted by power loss or cable pull leaves board recoverable over USB-C.
- FR52. Saved configuration survives firmware updates.

### Protocol interface
- FR53. Protocol spec is the single source of truth for every client. Covers bus and USB-C. For every command: name, parameters with units and ranges, reply, error replies, modes in which it is allowed. Also lists every fault code, every status field, and the 1 kHz cyclic exchange format.
- FR54. Spec covers every capability in this document: setup, tuning, save, ID assignment, discovery, set-zero, homing, mode and setpoint commands, motor off (Idle), KILL, fault clear, firmware update.
- FR55. CLI tool (PC and Mac) sends any command in the spec to: one board over USB-C, or any node by ID on a chain through a store-bought isolated USB-to-CAN FD adapter plugged into the first board's bus-in. Decodes replies and fault codes into readable text (e.g. "node 3: FAULT bus overvoltage, 53.1 V > 52.0 V limit").
- FR56. Firmware behavior matches the spec. Any firmware change to a command updates the spec in the same release.

## Non-Functional Requirements
- NFR1. Size: board 45 × 45 mm, height ≤ 15 mm including connectors. Does not overhang a 5010-class motor (about 58 mm OD).
- NFR2. Mounting: encoder IC at board center on the motor-facing side, magnet-to-IC gap per IC spec (about 1 to 2 mm). 4 × M2.5 holes in the board corners, independent of the motor's hole pattern; the actuator's standoffs set the board-to-motor spacing (magnet gap and the clearance under the connector edge).
- NFR3. Connectors: all on one board edge, opposite the motor phase terminals and power stage, so every cable leaves the controller from one end. Side facing away from the motor: power IN, power OUT. Motor-facing side: CAN IN under power IN, USB-C, CAN OUT under power OUT, in that order. Motor phase pads side by side on the opposite edge, between the two corner holes.
- NFR4. Cost: ≤ US$80 per board including parts and fab assembly at qty 10, excluding motor and magnet.
- NFR5. Build: first run 10 boards, assembled by a PCB fab service.

## Out of Scope
- Sensorless and hall-sensor operation.
- External or output-side encoder input.
- On-board brake resistor (external regen clamp module instead, FR33).
- Active bus repeating; bus power switching on KILL.
- Reverse-polarity protection circuitry.
- On-board inrush limiting and hot-plugging boards into a live chain.
- Chain access through USB-C.
- Host software beyond the CLI tool: Python library, GUI, ROS driver (future work, built against the protocol spec).
- Custom USB-to-bus host adapter.
- On-board galvanic isolation of USB or CAN.
- Motor temperature sensing (thermistor input).
- Motors outside FR1 envelope.

## Open Questions
- None.

# FOC controller for a 7025 drone motor

Field-oriented control of a 7025 hobby motor (420 kV, 28 poles) with current,
speed and position loops, running at 20 kHz on an STM32, with space-vector
modulation, planned position moves and a measured cogging-torque table.

## Hardware

| Part | Used here |
|---|---|
| Controller | NUCLEO-F446RE |
| Driver | BOOSTXL-DRV8305EVM (3-PWM mode, low-side shunts) |
| Encoder | AS5048A over SPI, powered from 3.3 V |
| Supply | 12 V 10 A mains adapter (cannot absorb braking energy) |

Wiring, Nucleo Arduino-header names:

| Signal | Pin |
|---|---|
| DRV8305 PWMHA / PWMHB / PWMHC | D7 / D8 / D2 |
| DRV8305 SCS, ENGATE, FAULT | D9, D6, D5 |
| DRV8305 ISENA / ISENB / ISENC, VSENPVDD | A0 / A1 / A2, A3 |
| AS5048A CSn | D10 |
| Shared SPI: SCK, MISO, MOSI | D13, D12, D11 |
| AS5048A 3.3V and 5V pins | both to Nucleo 3V3 |

Leave the DRV8305 board's own 3.3V pin unconnected.

## Layout

- `nucleo_foc/` — the controller. This is the sketch to use.
- `legacy/` — earlier steps, kept for reference: Arduino Uno bring-up and
  control sketches (`drv8305_*`, `encoder_read`, ...), tests for a
  DRV8323RS board that turned out to have a faulty phase, and the Nucleo
  bring-up sketches. `legacy/nucleo_check` (wiring check, no PWM) and
  `legacy/nucleo_pwm_test` (resistance and inductance measurement) are still
  useful when changing hardware.
- `docs/` — driver and evaluation-board datasheets.

## Running it

Arduino IDE with the "STM32 MCU based boards" package and STM32CubeProgrammer.
Tools: Board **Nucleo-64**, Board part number **Nucleo F446RE**, Upload method
**STM32CubeProgrammer (SWD)**. Serial Monitor at 115200 baud, line ending
**Newline**.

Clamp the motor, nothing on the shaft, then type `g`.

| Command | Effect |
|---|---|
| `p<deg>` | planned move to an angle (accelerate, cruise, brake), then hold; prints the time taken |
| `v<rpm>` | speed control (up to 1000) |
| `q<amps>`, `d<amps>` | torque current, and d-axis current (no torque) |
| `s` | hold the present position |
| `x` | outputs off, motor coasts |
| `a` | re-measure encoder offset and direction |
| `C` | scan cogging torque and store the table in flash |
| `t` | measure acceleration per amp and retune the outer loops |
| `c0` / `c1` | cogging and friction feed-forward off / on |
| `W`, `R`, `B`, `O`, `D`, `P`, `K`, `I` | loop bandwidths, spacing and gains |
| `m<rows>` | status rows per second, `m0` for none |

Settings typed at run time are lost on reset; the defaults live at the top of
the sketch. The cogging table is the exception: it is kept in flash.

## Measured values (the sketch's defaults)

| Quantity | Value |
|---|---|
| Phase resistance, inductance | 0.030 Ω, 5.5 µH |
| Pole pairs | 14 |
| Back-EMF | about 1.75 mV per rpm (phase amplitude) |
| Acceleration per amp, bare shaft | 2900 rpm/s per A |
| Loop bandwidths | current 500 Hz, speed 50 Hz, position 5 Hz |
| Limits | 12 A target, 18 A trip, 1000 rpm, 3 V phase voltage |
| Planned moves | up to 900 rpm, accelerate 20,000 rpm/s, brake 8,000 rpm/s |

## When something changes

- **Motor wires moved, or encoder magnet remounted:** run `a`, update
  `encoderOffset` and `direction` in the sketch, then run `C`.
- **Anything attached to the shaft:** run `t`; the stored tuning is for a bare
  rotor.
- **Different supply:** braking energy is burned in the motor windings because
  this adapter shuts down when current flows back into it. The thresholds
  (`BUS_DUMP_START_VOLTS` and neighbours) assume a 12 V bus.

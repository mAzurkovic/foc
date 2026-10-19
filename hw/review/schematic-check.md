# Schematic check: foc_controller

Generated from KiCad's own netlist export (`kicad-cli sch export netlist`), not from the generator, so every row reflects what KiCad will hand to the PCB. "Present" is checked against that netlist.

## Status
- ERC: 0 violations (`tools/render hw/foc_controller.kicad_pro`), no exclusions.
- 119 components, 106 nets. One DNP: J6 SWD pads (debug only, not fitted). The RC snubber pads were removed on 2026-10-19 (ringing handled by a tight hot loop in layout and DRV8353S IDRIVE) and R/C references renumbered to close the gap.
- Every BOM part has an LCSC number. Mechanical items carry none and are excluded from the BOM: H1-H8 mounting holes, TP1-TP8, J3-J5 motor holes, J6, JP1, NT1-NT6.
- Polarized parts verified from the netlist: D1 K = BUCK_SW, A = GND; D2/D3 A = +5V_BUCK / +5V_USB, K = +5V; C1-C4 + = VBUS; D4 anodes from R12/R13; Q1/Q3/Q5 D = VBUS, Q2/Q4/Q6 S = shunt.

## Pin cross-check (datasheet function, net, required parts)
| Ref | Pin | Function | Net | Required | Present | Source |
|---|---|---|---|---|---|---|
| U1 | 8 | VIN | VBUS | 1 uF 100 V + 0.1 uF 100 V to GND; RT to RT/SD | yes | LM5008A SNVS583H Fig. 10, Table 1 |
| U1 | 6 | RT/SD (on-time) | BUCK_RT | RT 180k to VIN | yes | LM5008A SNVS583H Eq. 4 |
| U1 | 3 | RCL | BUCK_RCL | RCL 383k to RTN | yes | LM5008A SNVS583H Eq. 5 |
| U1 | 4 | RTN | GND | GND | yes | LM5008A SNVS583H pin table |
| U1 | 7 | VCC | BUCK_VCC | 0.47 uF to GND | yes | LM5008A SNVS583H 8.2.2.2 C3 |
| U1 | 2 | BST | BUCK_BST | 0.01 uF BST-SW | yes | LM5008A SNVS583H pin table |
| U1 | 1 | SW | BUCK_SW | inductor, Schottky K, BST cap | yes | LM5008A SNVS583H pin table |
| U1 | 5 | FB | BUCK_FB | divider RFB2 to VOUT, RFB1 to GND | yes | LM5008A SNVS583H Eq. 3, Fig. 10 |
| U2 | 1 | VIN | +5V | 1 uF X7R | yes | AP2112 typical application, Note 4 |
| U2 | 3 | EN | +5V | tied to VIN (internal 3M pull-down) | yes | AP2112 pin table |
| U2 | 5 | VOUT | +3V3 | 1 uF X7R | yes | AP2112 typical application |
| U2 | 4 | NC | unconnected-(U2-NC-Pad4) | no connect | yes | AP2112 pin table |
| U3 | 3 | VM | VBUS | 0.1 uF VM-rated + >= 10 uF | yes | DRV8353 SLVSDY6A Table 21 |
| U3 | 4 | VDRAIN | VBUS | to high-side drains (VBUS) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 5 | VCP | DRV_VCP | 1 uF 16 V to VDRAIN | yes | DRV8353 SLVSDY6A pin table, Table 21 |
| U3 | 2 | CPH | DRV_CPH | 47 nF VDRAIN-rated to CPL | yes | DRV8353 SLVSDY6A Table 21 |
| U3 | 1 | CPL | DRV_CPL | 47 nF to CPH | yes | DRV8353 SLVSDY6A Table 21 |
| U3 | 40 | VGLS | DRV_VGLS | 1 uF 16 V to GND | yes | DRV8353 SLVSDY6A Table 21 |
| U3 | 38 | DVDD | DRV_DVDD | 1 uF 6.3 V to GND | yes | DRV8353 SLVSDY6A Table 21 |
| U3 | 24 | VREF | +3V3 | 0.1 uF to AGND | yes | DRV8353 SLVSDY6A pin table |
| U3 | 25 | AGND | GND | GND | yes | DRV8353 SLVSDY6A pin table |
| U3 | 39 | GND | GND | GND | yes | DRV8353 SLVSDY6A pin table |
| U3 | 41 | EP | GND | GND | yes | DRV8353 SLVSDY6A package |
| U3 | 26 | nFAULT | DRV_nFAULT | pull-up (open drain) | yes | DRV8353 SLVSDY6A Fig. 63 (10k) |
| U3 | 27 | SDO | DRV_SDO | pull-up (open drain) | yes | DRV8353 SLVSDY6A Fig. 63 (1k) |
| U3 | 28 | SDI | DRV_SDI | MCU SPI1 MOSI PA7 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 29 | SCLK | DRV_SCLK | MCU SPI1 SCK PA5 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 30 | nSCS | DRV_nSCS | MCU PB7 (FT, internal pull-up to DVDD) | yes | DRV8353 SLVSDY6A pin table, DS12288 |
| U3 | 31 | ENABLE | DRV_EN | MCU PB11 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 32 | INHA | INHA | TIM1_CH1 PA8 | yes | DRV8353 SLVSDY6A 6x PWM |
| U3 | 33 | INLA | INLA | TIM1_CH1N PB13 | yes | DRV8353 SLVSDY6A 6x PWM |
| U3 | 34 | INHB | INHB | TIM1_CH2 PA9 | yes | DRV8353 SLVSDY6A 6x PWM |
| U3 | 35 | INLB | INLB | TIM1_CH2N PB14 | yes | DRV8353 SLVSDY6A 6x PWM |
| U3 | 36 | INHC | INHC | TIM1_CH3 PA10 | yes | DRV8353 SLVSDY6A 6x PWM |
| U3 | 37 | INLC | INLC | TIM1_CH3N PB15 | yes | DRV8353 SLVSDY6A 6x PWM |
| U3 | 6 | GHA | GHA | gate Q1 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 7 | SHA | PHASE_A | source Q1 / phase | yes | DRV8353 SLVSDY6A pin table |
| U3 | 8 | GLA | GLA | gate Q2 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 9 | SPA | A_SENSE_P | shunt top Kelvin tap (NT1) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 10 | SNA | A_SENSE_N | shunt bottom Kelvin tap (NT2) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 15 | GHB | GHB | gate Q3 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 14 | SHB | PHASE_B | source Q3 / phase | yes | DRV8353 SLVSDY6A pin table |
| U3 | 13 | GLB | GLB | gate Q4 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 12 | SPB | B_SENSE_P | shunt top Kelvin tap (NT3) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 11 | SNB | B_SENSE_N | shunt bottom Kelvin tap (NT4) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 16 | GHC | GHC | gate Q5 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 17 | SHC | PHASE_C | source Q5 / phase | yes | DRV8353 SLVSDY6A pin table |
| U3 | 18 | GLC | GLC | gate Q6 | yes | DRV8353 SLVSDY6A pin table |
| U3 | 19 | SPC | C_SENSE_P | shunt top Kelvin tap (NT5) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 20 | SNC | C_SENSE_N | shunt bottom Kelvin tap (NT6) | yes | DRV8353 SLVSDY6A pin table |
| U3 | 23 | SOA | SOA | 100R/1nF to ADC + COMP | yes | design D7/OC |
| U3 | 22 | SOB | SOB | 100R/1nF to ADC + COMP | yes | design D7/OC |
| U3 | 21 | SOC | SOC | 100R/1nF to ADC + COMP | yes | design D7/OC |
| U4 | 1 | VBAT | +3V3 | to VDD | yes | STM32G474 DS12288 3.11.6 |
| U4 | 23 | VDD | +3V3 | 100 nF each + 4.7 uF | yes | STM32G474 DS12288 Fig. 16 |
| U4 | 35 | VDD | +3V3 |  | yes | STM32G474 DS12288 Fig. 16 |
| U4 | 48 | VDD | +3V3 |  | yes | STM32G474 DS12288 Fig. 16 |
| U4 | 21 | VDDA | +3V3 | 10 nF + 1 uF | yes | STM32G474 DS12288 Fig. 16 |
| U4 | 20 | VREF+ | +3V3 | 100 nF + 1 uF | yes | STM32G474 DS12288 Fig. 16 |
| U4 | 49 | VSS (EP) | GND | GND | yes | STM32G474 DS12288 Fig. 5 |
| U4 | 7 | PG10-NRST | NRST | 100 nF to GND | yes | STM32G474 DS12288 Fig. 27 |
| U4 | 46 | PB8-BOOT0 | BOOT0 | 10k pull-down, JP1 to 3V3 | yes | STM32G474 DS12288 3.7 |
| U4 | 5 | PF0-OSC_IN | XTAL_IN | crystal + 10 pF C0G | yes | STM32G474 DS12288 Table 41, AN2867 |
| U4 | 6 | PF1-OSC_OUT | XTAL_OUT | crystal + 10 pF C0G | yes | STM32G474 DS12288 Table 41 |
| U4 | 8 | PA0 | SOA_ADC | SOA_ADC (ADC1, COMP3_INP) | yes | STM32G474 DS12288 pin table |
| U4 | 9 | PA1 | SOB_ADC | SOB_ADC (ADC2, COMP1_INP) | yes | STM32G474 DS12288 pin table |
| U4 | 17 | PB0 | SOC_ADC | SOC_ADC (ADC3, COMP4_INP) | yes | STM32G474 DS12288 pin table |
| U4 | 10 | PA2 | VBUS_ADC | VBUS_ADC | yes | STM32G474 DS12288 pin table |
| U4 | 11 | PA3 | NTC_SENSE | NTC_SENSE | yes | STM32G474 DS12288 pin table |
| U4 | 13 | PA5 | DRV_SCLK | SPI1_SCK | yes | STM32G474 DS12288 AF table |
| U4 | 14 | PA6 | DRV_SDO | SPI1_MISO | yes | STM32G474 DS12288 AF table |
| U4 | 15 | PA7 | DRV_SDI | SPI1_MOSI | yes | STM32G474 DS12288 AF table |
| U4 | 45 | PB7 | DRV_nSCS | DRV nSCS (FT_f) | yes | STM32G474 DS12288 pin table |
| U4 | 24 | PB11 | DRV_EN | DRV ENABLE | yes | STM32G474 DS12288  |
| U4 | 22 | PB10 | DRV_nFAULT | DRV nFAULT | yes | STM32G474 DS12288  |
| U4 | 39 | PC10 | ENC_SCK | SPI3_SCK | yes | STM32G474 DS12288 AF table |
| U4 | 40 | PC11 | ENC_MISO | SPI3_MISO | yes | STM32G474 DS12288 AF table |
| U4 | 43 | PB5 | ENC_MOSI | SPI3_MOSI | yes | STM32G474 DS12288 AF table |
| U4 | 25 | PB12 | ENC_CS | ENC CSn | yes | STM32G474 DS12288  |
| U4 | 38 | PA15 | CAN_TX | FDCAN3_TX | yes | STM32G474 DS12288 AF table |
| U4 | 41 | PB3 | CAN_RX | FDCAN3_RX | yes | STM32G474 DS12288 AF table |
| U4 | 47 | PB9 | CAN_STB | CAN STB | yes | STM32G474 DS12288  |
| U4 | 33 | PA11 | USB_DM | USB_DM | yes | STM32G474 DS12288 AF table |
| U4 | 34 | PA12 | USB_DP | USB_DP | yes | STM32G474 DS12288 AF table |
| U4 | 36 | PA13 | SWDIO | SWDIO | yes | STM32G474 DS12288  |
| U4 | 37 | PA14 | SWCLK | SWCLK | yes | STM32G474 DS12288  |
| U4 | 18 | PB1 | LED_G | LED green | yes | STM32G474 DS12288  |
| U4 | 19 | PB2 | LED_R | LED red | yes | STM32G474 DS12288  |
| U5 | 11 | VDD | +3V3 | 3.3 V mode: tied to VDD3V3, 100 nF | yes | AS5047P DS000324 Fig. 10 |
| U5 | 12 | VDD3V3 | +3V3 | tied to VDD | yes | AS5047P DS000324 Fig. 10 |
| U5 | 13 | GND | GND | GND | yes | AS5047P DS000324 pin table |
| U5 | 5 | TEST | GND | to GND | yes | AS5047P DS000324 pin table |
| U5 | 1 | CSn | ENC_CS | MCU PB12 | yes | AS5047P DS000324 pin table |
| U5 | 2 | CLK | ENC_SCK | MCU PC10 | yes | AS5047P DS000324 pin table |
| U5 | 3 | MISO | ENC_MISO | MCU PC11 | yes | AS5047P DS000324 pin table |
| U5 | 4 | MOSI | ENC_MOSI | MCU PB5 | yes | AS5047P DS000324 pin table |
| U6 | 3 | VCC | +3V3 | 100 nF | yes | TCAN3413 SLLSFS8A 8.4 |
| U6 | 5 | VIO | +3V3 | 100 nF | yes | TCAN3413 SLLSFS8A 8.4 |
| U6 | 2 | GND | GND | GND | yes | TCAN3413 SLLSFS8A pin table |
| U6 | 9 | PAD | GND | GND | yes | TCAN3413 SLLSFS8A pin table |
| U6 | 1 | TXD | CAN_TX | MCU PA15 (internal pull-up) | yes | TCAN3413 SLLSFS8A pin table |
| U6 | 4 | RXD | CAN_RX | MCU PB3 | yes | TCAN3413 SLLSFS8A pin table |
| U6 | 8 | STB | CAN_STB | MCU PB9 (internal pull-up) | yes | TCAN3413 SLLSFS8A pin table |
| U6 | 7 | CANH | CAN_H | J7/J8 pin 1, 120R + SW1 | yes | TCAN3413 SLLSFS8A 8.2.1.1 |
| U6 | 6 | CANL | CAN_L | J7/J8 pin 2, SW1 | yes | TCAN3413 SLLSFS8A 8.2.1.1 |
| U7 | 1 | I/O1 | USB_DP | USB_DP | yes | USBLC6-2SC6 pin table |
| U7 | 6 | I/O1 | USB_DP | USB_DP | yes | USBLC6-2SC6 pin table |
| U7 | 3 | I/O2 | USB_DM | USB_DM | yes | USBLC6-2SC6 pin table |
| U7 | 4 | I/O2 | USB_DM | USB_DM | yes | USBLC6-2SC6 pin table |
| U7 | 5 | VBUS | +5V_USB | +5V_USB | yes | USBLC6-2SC6 pin table |
| U7 | 2 | GND | GND | GND | yes | USBLC6-2SC6 pin table |
| Y1 | 1 | crystal | XTAL_IN | PF0 | yes | YSX321SL drawing (pins 2/4 GND) |
| Y1 | 3 | crystal | XTAL_OUT | PF1 | yes | YSX321SL drawing (pins 2/4 GND) |
| Y1 | 2 | GND | GND | GND | yes | YSX321SL drawing (pins 2/4 GND) |
| Y1 | 4 | GND | GND | GND | yes | YSX321SL drawing (pins 2/4 GND) |

## Part ratings vs stress
| Part | Rating | Stress | Margin | Source |
|---|---|---|---|---|
| Q1-Q6 BSC026N08NS5 | 80 V | 54 V hard trip | 1.48x (rule 1.5x: 81 V at 54 V, accepted) | BSC026 datasheet |
| U3 DRV8353S VM | 80 V abs max, 75 V rec | 54 V | 21 V | SLVSDY6A 7.1/7.3 |
| U1 LM5008A VIN | 95 V | 54 V | 41 V | SNVS583H |
| C1-C4 polymer | 80 V, 2.7 A each | 54 V; ~13 A total at 20 A RMS with ceramics | 1.48x | JLC listing (no datasheet on LCSC) |
| C5-C10, C11, C12, C18, C20 | 100 V X7R | 54 V | 1.85x | rule: 100 V on 48 V bus |
| R1 (RT) / R17 (VBUS top) 0603 | 75 V | ~50 V across | ok | rule: 0603 on 52 V |
| D1 DFLS1100-7 | 100 V / 1 A | 54 V, 610 mA ILIM | ok | SNVS583H 8.2.2.2 |
| D2/D3 B5819W | 40 V / 1 A | 5 V, <0.3 A | ok | |
| R8-R10 shunt | 3 W | 20 A RMS: 0.4 W; 40 A peak 2 s: 1.6 W | ok | |
| U2 AP2112K | 600 mA, 6 V max in | 5 V, ~145 mA | ok | Diodes datasheet |

## DC-bias (value-critical ceramics)
Estimates from typical X7R/X5R curves for the package and voltage; manufacturer curves were not available for the JLC parts. Verify on the first board where noted.
- C19/C21 1 uF 50 V 0805 X7R at ~11 V: about 0.8 uF effective; datasheet asks for 1 uF nominal. OK.
- C15 22 uF 25 V 1206 X5R at 5 V: about 11 uF effective. Sets output ripple with R5 (TI used 22 uF at 10 V, a similar effective value).
- C5-C10 4.7 uF 100 V 1210 X7R at 48 V: about 2 uF each, 12 uF total; the polymers carry the bulk.
- C16/C17 1 uF 50 V 0805 X7R at 5 V / 3.3 V: about 0.9 uF. AP2112 needs 1 uF nominal. OK.

## Analyzer (kicad-happy analyze_schematic.py) triage
| Finding | Verdict |
|---|---|
| LR-001 LED D4 has no current-limiting resistor | False positive: R12/R13 in series via LED_G_A / LED_R_A (netlist). |
| VM-001 USB_DP/USB_DM 5 V / 3.3 V crossing | False positive: the 5 V is the USBLC6 clamp rail, not a signal driver. |
| RS-001 +5V_USB, BUCK_VCC, VBUS_ADC have no declared source | Not rails: USB connector input, LM5008A internal regulator pin, ADC divider node. ERC clean. |
| SJ-DET JP1 open by default | Intended: BOOT0 recovery jumper. |
| UC-001 no VBUS decoupling at J9 | Fixed: C43 1 uF added. |
| FS-001 feedback divider 500 Ohm | Kept: matches TI's example scale (3.01k / 1k); 2.5 mA from a 350 mA buck. |
| XT-001 crystal load 8 pF vs 9 pF | Analyzer assumes 3 pF stray; design assumes 4 pF. Check HSE on first board (design OQ4). |
- PCB, EMC, thermal, Gerber analyzers: not applicable until layout.

## SPICE (ngspice, kicad-happy simulate_subcircuits.py, results in hw/review/spice.json)
17 subcircuits: 15 pass, 1 warn, 0 fail, 1 skip (rerun 2026-10-19 after the snubber removal; the three snubber RC rows dropped out).
| Circuit | Expected | Simulated | Result |
|---|---|---|---|
| R3/R4 LM5008A feedback | FB 2.5 V at 5.0 V | 2.500 V | pass |
| R17/R18 VBUS divider | 0.0485 | 0.0485 | pass |
| R14-R16 / C35-C37 CSA filters | 1.59 MHz | 1.588 MHz | pass |
| R19/C39 NTC filter | 15.9 kHz | 15.88 kHz | pass |
| Decoupling groups | low Z | 0.9 to 160 mOhm at 1 MHz | pass |
| 3.3 V inrush | 3.3 V | 3.30 V, 56 mA | pass |
| Y1 load | 9 pF | 8 pF at 3 pF stray | pass (9 pF at 4 pF stray; measure HSE on first board) |
| C38 | decoupling | high Z | false positive: ADC filter cap, not decoupling |
| Q1-Q6 bridge | | skipped | no MOSFET model in the analyzer |

## Found and fixed during the check
- kicad-sch-api computes pin positions for 90/270-degree rotations mirrored relative to KiCad. D1, D2, D3 were reversed in the first netlist. Fixed in the generator and re-verified from the KiCad netlist above.

## Open items
- XT30 + / - pad assignment comes from the LCSC footprint; confirm on a physical connector before layout.
- Design doc pin map update: DRV nSCS on PB7 (PB12 is TT_a, not 5 V tolerant), encoder CSn on PB12.

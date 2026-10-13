// BOOSTXL-DRV8305EVM bring-up, motor disconnected.
//
// Part 1 (gate driver disabled): talk to the DRV8305 over SPI, check a few
// power-on defaults and switch it to 3-PWM mode.
// Part 2 (gate driver enabled, all PWM inputs low): print the three
// current-sense outputs and the bus-voltage sense. In 3-PWM mode a low input
// turns that phase's low-side MOSFET on, so this holds all three phases at
// ground; the INL pins are ignored.
//
// Wiring (Uno). Positions are counted from the terminal-block end of the board.
//   Left header, edge column:   1st 3.3V (leave unconnected), 3rd nFAULT -> D8, 7th SCLK -> D13
//   Left header, inner column:  2nd GND -> GND, 6th VSEN_PVDD -> A3,
//                               7th/8th/9th ISEN_A/B/C -> A0, A1, A2
//   Right header, edge column:  2nd SCS -> D6, 6th SDI -> D11, 7th SDO -> D12, 8th EN_GATE -> D7
//   Right header, inner column: 1st/3rd/5th INH_A/B/C -> D9, D10, D3
//   AS5048A CSn -> D2 (held high here so it stays off the bus)

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = 2;
const uint8_t PIN_DRIVER_CS = 6;
const uint8_t PIN_EN_GATE = 7;
const uint8_t PIN_NFAULT = 8;
const uint8_t PINS_INH[] = {9, 10, 3};
const uint8_t PINS_ISEN[] = {A0, A1, A2};
const uint8_t PIN_VSEN_BUS = A3;

const uint8_t REG_WARNINGS = 0x1;
const uint8_t REG_VDS_FAULTS = 0x2;
const uint8_t REG_IC_FAULTS = 0x3;
const uint8_t REG_VGS_FAULTS = 0x4;
const uint8_t REG_HS_GATE_DRIVE = 0x5;
const uint8_t REG_GATE_DRIVE = 0x7;
const uint8_t REG_SHUNT_AMP = 0xA;
const uint8_t REG_VDS_SENSE = 0xC;

const uint16_t DATA_MASK = 0x07FF;
const uint16_t READ_FLAG = 0x8000;

// Gate drive control: power-on default (0x216) with PWM_MODE = 3 inputs.
const uint16_t GATE_DRIVE_PWM_3X = 0x296;

const float ADC_REF_VOLTS = 5.0f;  // Uno default reference (USB 5 V, so approximate)
const uint16_t ADC_COUNTS = 1024;
const uint16_t SAMPLES = 256;
const float BUS_DIVIDER = (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM

// DRV8305: SPI mode 1, MSB first.
const SPISettings DRIVER_SPI(100000, MSBFIRST, SPI_MODE1);

uint16_t transfer(uint16_t out) {
  SPI.beginTransaction(DRIVER_SPI);
  digitalWrite(PIN_DRIVER_CS, LOW);
  uint16_t in = SPI.transfer16(out);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);  // nSCS must stay high >= 500 ns between frames
  return in & DATA_MASK;
}

uint16_t readRegister(uint8_t reg) {
  return transfer(READ_FLAG | ((uint16_t)reg << 11));
}

void writeRegister(uint8_t reg, uint16_t data) {
  transfer(((uint16_t)reg << 11) | (data & DATA_MASK));
}

void printHex(uint16_t value) {
  Serial.print(F("0x"));
  if (value < 0x100) Serial.print('0');
  if (value < 0x10) Serial.print('0');
  Serial.print(value, HEX);
}

// Prints the register and returns true if the bits under `mask` hold the expected value.
bool checkRegister(const __FlashStringHelper *name, uint8_t reg, uint16_t expected, uint16_t mask = DATA_MASK) {
  uint16_t value = readRegister(reg) & mask;
  Serial.print(name);
  Serial.print(F(" = "));
  printHex(value);
  Serial.print(F(" (expected "));
  printHex(expected);
  Serial.println(value == expected ? F(") ok") : F(") MISMATCH"));
  return value == expected;
}

void printStatus() {
  Serial.print(digitalRead(PIN_NFAULT) ? F("nFAULT ok ") : F("nFAULT LOW"));
  Serial.print(F("  warn="));
  printHex(readRegister(REG_WARNINGS));
  Serial.print(F(" vds="));
  printHex(readRegister(REG_VDS_FAULTS));
  Serial.print(F(" ic="));
  printHex(readRegister(REG_IC_FAULTS));
  Serial.print(F(" vgs="));
  printHex(readRegister(REG_VGS_FAULTS));
}

// Mean of SAMPLES conversions, in ADC counts.
float readAveraged(uint8_t pin) {
  analogRead(pin);  // discard the first conversion after switching channel
  uint32_t sum = 0;
  for (uint16_t i = 0; i < SAMPLES; i++) sum += analogRead(pin);
  return (float)sum / SAMPLES;
}

void printCurrentChannel(const __FlashStringHelper *name, uint8_t pin) {
  float counts = readAveraged(pin);
  Serial.print(name);
  Serial.print(counts, 1);
  Serial.print(F(" ("));
  Serial.print(counts * ADC_REF_VOLTS / ADC_COUNTS, 3);
  Serial.print(F(" V)  "));
}

void setup() {
  Serial.begin(115200);

  digitalWrite(PIN_EN_GATE, LOW);
  pinMode(PIN_EN_GATE, OUTPUT);
  for (uint8_t pin : PINS_INH) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  }

  digitalWrite(PIN_ENCODER_CS, HIGH);
  pinMode(PIN_ENCODER_CS, OUTPUT);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  pinMode(PIN_DRIVER_CS, OUTPUT);
  pinMode(PIN_NFAULT, INPUT);
  SPI.begin();
  delay(10);

  Serial.println(F("\n--- DRV8305 check ---"));
  Serial.println(F("Power-on defaults:"));
  bool ok = true;
  ok &= checkRegister(F("  HS gate drive"), REG_HS_GATE_DRIVE, 0x344);
  ok &= checkRegister(F("  shunt amp    "), REG_SHUNT_AMP, 0x000);
  // Bits 10:8 are reserved and bit 9 reads back as 1 on the real chip, so compare only the defined fields.
  ok &= checkRegister(F("  VDS sense    "), REG_VDS_SENSE, 0x0C8, 0x0FF);

  Serial.println(F("Setting 3-PWM mode:"));
  writeRegister(REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);
  ok &= checkRegister(F("  gate drive   "), REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);

  if (!ok) {
    Serial.println(F("RESULT: FAILED - check motor supply is on, then SPI wiring. Gate driver left disabled."));
    while (true) {}
  }
  Serial.println(F("RESULT: driver SPI OK, enabling gate driver"));

  digitalWrite(PIN_EN_GATE, HIGH);
  delay(10);
}

void loop() {
  printCurrentChannel(F("A: "), PINS_ISEN[0]);
  printCurrentChannel(F("B: "), PINS_ISEN[1]);
  printCurrentChannel(F("C: "), PINS_ISEN[2]);
  Serial.print(F("bus: "));
  Serial.print(readAveraged(PIN_VSEN_BUS) * ADC_REF_VOLTS / ADC_COUNTS * BUS_DIVIDER, 2);
  Serial.print(F(" V  "));
  printStatus();
  Serial.println();
  delay(1000);
}

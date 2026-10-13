// BOOSTXL-DRV8323RS bring-up: talk to the gate driver over SPI, switch it to
// 3x PWM mode and report faults. The power stage is never driven: PWM inputs
// stay low and INLx stay low (all phases Hi-Z).
//
// Wiring (Uno):
//   J4-1/5/9   INHA/B/C -> D9, D10, D3
//   J4-3/7/11  INLA/B/C -> D4 (joined), or D4, D5, A5 separately
//   J3-9   ENABLE -> D7
//   J3-15  nFAULT -> D8
//   J4-18  nSCS   -> D6
//   J3-13  SCLK   -> D13, J4-12 SDI -> D11, J4-14 SDO -> D12 (shared with encoder)
//   GND -> GND. The board's 3.3V pin stays unconnected.
//   AS5048A CSn -> D2 (held high here so it stays off the bus)

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = 2;
const uint8_t PIN_DRIVER_CS = 6;
const uint8_t PIN_ENABLE = 7;
const uint8_t PIN_NFAULT = 8;
const uint8_t PINS_INH[] = {9, 10, 3};
const uint8_t PINS_INL[] = {4, 5, A5};

const uint8_t REG_FAULT_STATUS_1 = 0x0;
const uint8_t REG_FAULT_STATUS_2 = 0x1;
const uint8_t REG_DRIVER_CONTROL = 0x2;
const uint8_t REG_GATE_DRIVE_HS = 0x3;
const uint8_t REG_GATE_DRIVE_LS = 0x4;
const uint8_t REG_OCP_CONTROL = 0x5;
const uint8_t REG_CSA_CONTROL = 0x6;

const uint16_t DATA_MASK = 0x07FF;
const uint16_t READ_FLAG = 0x8000;

const uint16_t DRIVER_CONTROL_CLR_FLT = 1 << 0;
const uint16_t DRIVER_CONTROL_PWM_3X = 1 << 5;

// DRV8323: SPI mode 1, MSB first. SDO is open drain, so keep the clock slow.
const SPISettings DRIVER_SPI(100000, MSBFIRST, SPI_MODE1);

uint16_t transfer(uint16_t out) {
  SPI.beginTransaction(DRIVER_SPI);
  digitalWrite(PIN_DRIVER_CS, LOW);
  uint16_t in = SPI.transfer16(out);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);  // nSCS must stay high >= 400 ns between words
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

// Prints the register and returns true if it holds the expected value.
bool checkRegister(const __FlashStringHelper *name, uint8_t reg, uint16_t expected) {
  uint16_t value = readRegister(reg);
  Serial.print(name);
  Serial.print(F(" = "));
  printHex(value);
  Serial.print(F(" (expected "));
  printHex(expected);
  Serial.println(value == expected ? F(") ok") : F(") MISMATCH"));
  return value == expected;
}

void printFaults() {
  Serial.print(F("nFAULT pin = "));
  Serial.print(digitalRead(PIN_NFAULT) ? F("high (ok)") : F("LOW (fault)"));
  Serial.print(F("  status1 = "));
  printHex(readRegister(REG_FAULT_STATUS_1));
  Serial.print(F("  status2 = "));
  printHex(readRegister(REG_FAULT_STATUS_2));
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  for (uint8_t pin : PINS_INH) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  }
  for (uint8_t pin : PINS_INL) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  }

  digitalWrite(PIN_ENCODER_CS, HIGH);
  pinMode(PIN_ENCODER_CS, OUTPUT);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  pinMode(PIN_DRIVER_CS, OUTPUT);
  pinMode(PIN_NFAULT, INPUT);
  SPI.begin();

  // Toggling ENABLE resets the registers to their defaults.
  pinMode(PIN_ENABLE, OUTPUT);
  digitalWrite(PIN_ENABLE, LOW);
  delay(5);
  digitalWrite(PIN_ENABLE, HIGH);
  delay(5);

  Serial.println(F("\n--- DRV8323RS check ---"));
  Serial.println(F("Power-on defaults:"));
  bool ok = true;
  ok &= checkRegister(F("  driver control"), REG_DRIVER_CONTROL, 0x000);
  ok &= checkRegister(F("  gate drive HS "), REG_GATE_DRIVE_HS, 0x3FF);
  ok &= checkRegister(F("  gate drive LS "), REG_GATE_DRIVE_LS, 0x7FF);
  ok &= checkRegister(F("  OCP control   "), REG_OCP_CONTROL, 0x159);
  ok &= checkRegister(F("  CSA control   "), REG_CSA_CONTROL, 0x283);

  Serial.println(F("Setting 3x PWM mode:"));
  writeRegister(REG_DRIVER_CONTROL, DRIVER_CONTROL_PWM_3X | DRIVER_CONTROL_CLR_FLT);
  ok &= checkRegister(F("  driver control"), REG_DRIVER_CONTROL, DRIVER_CONTROL_PWM_3X);

  Serial.println(ok ? F("RESULT: driver SPI OK")
                    : F("RESULT: FAILED - check motor supply is on, then SPI wiring"));
}

void loop() {
  printFaults();
  delay(1000);
}

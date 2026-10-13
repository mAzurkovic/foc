// NUCLEO-F446RE bring-up with the BOOSTXL-DRV8305EVM and the AS5048A, motor
// disconnected. Checks the wiring before any PWM code exists for this board:
//   1. reads the encoder
//   2. checks the DRV8305 over SPI and sets 3-PWM mode (gate driver disabled)
//   3. enables the gate driver with all PWM inputs low (all three low sides
//      on) and prints the current-sense outputs, bus voltage and fault registers
//
// Wiring (Nucleo Arduino-header names; MCU pins in brackets):
//   DRV8305 PWMHA / PWMHB / PWMHC -> D7 (PA8) / D8 (PA9) / D2 (PA10)   TIM1 CH1..3
//   DRV8305 SCS    -> D9  (PC7)
//   DRV8305 ENGATE -> D6  (PB10)
//   DRV8305 FAULT  -> D5  (PB4)
//   DRV8305 ISENA / ISENB / ISENC -> A0 (PA0) / A1 (PA1) / A2 (PA4)
//   DRV8305 VSENPVDD -> A3 (PB0)
//   AS5048A CSn    -> D10 (PB6)
//   Shared SPI: SCLK/SCK -> D13 (PA5), SDO/MISO -> D12 (PA6), SDI/MOSI -> D11 (PA7)
//   GND -> GND on both. The DRV8305 board's 3.3V pin stays unconnected.
//   AS5048A supply: 3.3 V from the Nucleo's 3V3 pin, on both its 3.3V and 5V pins.

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = D10;
const uint8_t PIN_DRIVER_CS = D9;
const uint8_t PIN_EN_GATE = D6;
const uint8_t PIN_NFAULT = D5;
const uint8_t PINS_INH[] = {D7, D8, D2};
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

const uint16_t DRIVER_DATA_MASK = 0x07FF;
const uint16_t DRIVER_READ_FLAG = 0x8000;
const uint16_t GATE_DRIVE_PWM_3X = 0x296;   // power-on default (0x216) with PWM_MODE = 3 inputs

const uint16_t ENCODER_READ_ANGLE = 0xFFFF;  // read 0x3FFF, with parity
const uint16_t ENCODER_DATA_MASK = 0x3FFF;
const uint16_t ENCODER_ERROR_FLAG = 0x4000;
const float ENCODER_COUNTS = 16384.0f;

const float ADC_REF_VOLTS = 3.3f;
const float ADC_COUNTS = 4096.0f;  // 12-bit
const uint16_t SAMPLES = 256;
const float BUS_DIVIDER = (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM

// Both chips: SPI mode 1, MSB first.
const SPISettings DRIVER_SPI(100000, MSBFIRST, SPI_MODE1);
const SPISettings ENCODER_SPI(1000000, MSBFIRST, SPI_MODE1);

uint16_t transfer(uint8_t csPin, const SPISettings &settings, uint16_t out) {
  SPI.beginTransaction(settings);
  digitalWrite(csPin, LOW);
  uint16_t in = SPI.transfer16(out);
  digitalWrite(csPin, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);  // both chips need chip select high >= 500 ns between words
  return in;
}

uint16_t readDriver(uint8_t reg) {
  return transfer(PIN_DRIVER_CS, DRIVER_SPI, DRIVER_READ_FLAG | ((uint16_t)reg << 11)) & DRIVER_DATA_MASK;
}

void writeDriver(uint8_t reg, uint16_t data) {
  transfer(PIN_DRIVER_CS, DRIVER_SPI, ((uint16_t)reg << 11) | (data & DRIVER_DATA_MASK));
}

// The encoder answers in the following frame, so send the command twice.
// Returns the whole response word, including the error flag.
uint16_t readEncoderWord() {
  transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE);
  return transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE);
}

void printHex(uint16_t value) {
  Serial.print(F("0x"));
  if (value < 0x100) Serial.print('0');
  if (value < 0x10) Serial.print('0');
  Serial.print(value, HEX);
}

// Prints the register and returns true if the bits under `mask` hold the expected value.
bool checkRegister(const char *name, uint8_t reg, uint16_t expected, uint16_t mask = DRIVER_DATA_MASK) {
  uint16_t value = readDriver(reg) & mask;
  Serial.print(name);
  Serial.print(F(" = "));
  printHex(value);
  Serial.print(F(" (expected "));
  printHex(expected);
  Serial.println(value == expected ? F(") ok") : F(") MISMATCH"));
  return value == expected;
}

// Mean of SAMPLES conversions, in volts at the pin.
float readVolts(uint8_t pin) {
  uint32_t sum = 0;
  for (uint16_t i = 0; i < SAMPLES; i++) sum += analogRead(pin);
  return (float)sum / SAMPLES * ADC_REF_VOLTS / ADC_COUNTS;
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);

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
  delay(100);

  Serial.println(F("\n--- Nucleo F446RE wiring check ---"));
  Serial.println(F("DRV8305 power-on defaults:"));
  bool ok = true;
  ok &= checkRegister("  HS gate drive", REG_HS_GATE_DRIVE, 0x344);
  ok &= checkRegister("  shunt amp    ", REG_SHUNT_AMP, 0x000);
  // Bits 10:8 are reserved and bit 9 reads back as 1 on the real chip, so compare only the defined fields.
  ok &= checkRegister("  VDS sense    ", REG_VDS_SENSE, 0x0C8, 0x0FF);

  Serial.println(F("Setting 3-PWM mode:"));
  writeDriver(REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);
  ok &= checkRegister("  gate drive   ", REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);

  if (!ok) {
    Serial.println(F("RESULT: FAILED - check motor supply is on, then SPI wiring. Gate driver left disabled."));
    while (true) {}
  }
  Serial.println(F("RESULT: driver SPI OK, enabling gate driver"));

  digitalWrite(PIN_EN_GATE, HIGH);
  delay(10);
}

void loop() {
  uint16_t word = readEncoderWord();
  Serial.print(F("enc: "));
  Serial.print(word & ENCODER_DATA_MASK);
  Serial.print(F(" ("));
  Serial.print((word & ENCODER_DATA_MASK) * (360.0f / ENCODER_COUNTS), 1);
  Serial.print(word & ENCODER_ERROR_FLAG ? F(" deg, ERROR FLAG)") : F(" deg)"));

  Serial.print(F("  A: "));
  Serial.print(readVolts(PINS_ISEN[0]), 3);
  Serial.print(F("  B: "));
  Serial.print(readVolts(PINS_ISEN[1]), 3);
  Serial.print(F("  C: "));
  Serial.print(readVolts(PINS_ISEN[2]), 3);
  Serial.print(F(" V  bus: "));
  Serial.print(readVolts(PIN_VSEN_BUS) * BUS_DIVIDER, 2);

  Serial.print(digitalRead(PIN_NFAULT) ? F(" V  nFAULT ok ") : F(" V  nFAULT LOW"));
  Serial.print(F("  warn="));
  printHex(readDriver(REG_WARNINGS));
  Serial.print(F(" vds="));
  printHex(readDriver(REG_VDS_FAULTS));
  Serial.print(F(" ic="));
  printHex(readDriver(REG_IC_FAULTS));
  Serial.print(F(" vgs="));
  printHex(readDriver(REG_VGS_FAULTS));
  Serial.println();
  delay(500);
}

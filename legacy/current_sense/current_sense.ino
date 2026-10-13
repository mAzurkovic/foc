// BOOSTXL-DRV8323RS current-sense check: enable the driver with every phase
// Hi-Z and print the three current-sense outputs and the bus-voltage sense.
// With no current flowing, each ISEN output should sit near 1.65 V (VREF / 2).
//
// Wiring (Uno): as driver_check, plus
//   J3-16/14/12 ISENA/B/C -> A4, A2, A1
//   J3-3        VSENVM    -> A3

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = 2;
const uint8_t PIN_DRIVER_CS = 6;
const uint8_t PIN_ENABLE = 7;
const uint8_t PIN_NFAULT = 8;
const uint8_t PINS_INH[] = {9, 10, 3};
const uint8_t PINS_INL[] = {4, 5, A5};
const uint8_t PINS_ISEN[] = {A4, A2, A1};
const uint8_t PIN_VSEN_BUS = A3;

const uint8_t REG_DRIVER_CONTROL = 0x2;
const uint8_t REG_CSA_CONTROL = 0x6;
const uint16_t DATA_MASK = 0x07FF;
const uint16_t READ_FLAG = 0x8000;
const uint16_t DRIVER_CONTROL_CLR_FLT = 1 << 0;
const uint16_t DRIVER_CONTROL_PWM_3X = 1 << 5;

// CSA control register: bidirectional (VREF / 2), 20 V/V, 1 V sense OCP.
const uint16_t CSA_DEFAULT = 0x283;
const uint16_t CSA_CAL_ALL = 0x01C;    // short every amplifier's inputs together
const uint16_t CSA_VREF_DIV = 0x200;   // clear for unidirectional (output idles near VREF)
const uint16_t CSA_GAIN_MASK = 0x0C0;  // clear for 5 V/V

const float ADC_REF_VOLTS = 5.0f;  // Uno default reference (USB 5 V, so approximate)
const uint16_t ADC_COUNTS = 1024;
const uint16_t SAMPLES = 256;

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

// Mean of SAMPLES conversions, in ADC counts.
float readAveraged(uint8_t pin) {
  analogRead(pin);  // discard the first conversion after switching channel
  uint32_t sum = 0;
  for (uint16_t i = 0; i < SAMPLES; i++) sum += analogRead(pin);
  return (float)sum / SAMPLES;
}

void printChannel(const __FlashStringHelper *name, uint8_t pin) {
  float counts = readAveraged(pin);
  Serial.print(name);
  Serial.print(counts, 1);
  Serial.print(F(" ("));
  Serial.print(counts * ADC_REF_VOLTS / ADC_COUNTS, 3);
  Serial.print(F(" V)  "));
}

void printCurrentChannels() {
  printChannel(F("A: "), PINS_ISEN[0]);
  printChannel(F("B: "), PINS_ISEN[1]);
  printChannel(F("C: "), PINS_ISEN[2]);
}

// Reads the amplifiers under one CSA configuration, then restores the default.
void diagnose(const __FlashStringHelper *name, uint16_t csaControl) {
  writeRegister(REG_CSA_CONTROL, csaControl);
  delay(20);
  Serial.print(name);
  printCurrentChannels();
  Serial.println();
  writeRegister(REG_CSA_CONTROL, CSA_DEFAULT);
  delay(20);
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

  writeRegister(REG_DRIVER_CONTROL, DRIVER_CONTROL_PWM_3X | DRIVER_CONTROL_CLR_FLT);

  Serial.println(F("\n--- current sense check (all phases Hi-Z) ---"));
  Serial.println(readRegister(REG_DRIVER_CONTROL) == DRIVER_CONTROL_PWM_3X
                     ? F("driver in 3x PWM mode")
                     : F("DRIVER NOT RESPONDING - check motor supply"));

  // Each amplifier should follow these settings; one that does not is not
  // reaching its ADC pin.
  diagnose(F("normal         (~325): "), CSA_DEFAULT);
  diagnose(F("inputs shorted (~325): "), CSA_DEFAULT | CSA_CAL_ALL);
  diagnose(F("unidirectional (~615): "), CSA_DEFAULT & ~CSA_VREF_DIV);
  diagnose(F("gain 5 V/V     (~325): "), CSA_DEFAULT & ~CSA_GAIN_MASK);
  Serial.println();
}

void loop() {
  printCurrentChannels();
  printChannel(F("bus: "), PIN_VSEN_BUS);
  Serial.println(digitalRead(PIN_NFAULT) ? F("nFAULT ok") : F("nFAULT LOW"));
  delay(500);
}

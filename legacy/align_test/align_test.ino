// First powered test: push a small DC current into phase A and out through
// phases B and C, ramping slowly until it reaches TARGET_AMPS.
//
// This pulls the rotor to electrical angle zero, so it gives us three things:
//   - proof that the power stage and the B/C current sensors work
//   - an estimate of the motor's phase resistance
//   - the encoder reading at electrical zero (the alignment offset)
//
// Phase A is PWM'd with very short pulses; B and C are held low the whole
// time, so their low-side shunts carry continuous current and can be sampled
// without synchronising to the PWM. Phase A's own current sensor is not used.
//
// Wiring (Uno): as current_sense, with the motor's three wires on the driver
// board's phase terminals. Type 'g' in the Serial Monitor to start.

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = 2;
const uint8_t PIN_DRIVER_CS = 6;
const uint8_t PIN_ENABLE = 7;
const uint8_t PIN_NFAULT = 8;
const uint8_t PIN_INH_A = 9;  // OC1A
const uint8_t PIN_INH_B = 10;
const uint8_t PIN_INH_C = 3;
const uint8_t PINS_INL[] = {4, 5, A5};
const uint8_t PIN_ISEN_B = A2;
const uint8_t PIN_ISEN_C = A1;
const uint8_t PIN_VSEN_BUS = A3;

const uint8_t POLE_PAIRS = 14;
const uint16_t ENCODER_COUNTS = 16384;

const float TARGET_AMPS = 2.0f;        // stop ramping once phase A reaches this
const float PHASE_LIMIT_AMPS = 3.0f;   // abort if B or C alone exceeds this
const uint16_t MAX_PULSE = 80;         // hard cap on duty: 80 / 2000 = 4 %
const uint16_t PWM_TOP = 2000;         // 16 MHz / (2 * 2000) = 4 kHz, 125 ns per count

const float ADC_REF_VOLTS = 5.0f;      // Uno default reference (USB 5 V, so approximate)
const float SHUNT_OHMS = 0.007f;       // assumed, same as the DRV8305 board: not yet confirmed
const float CSA_GAIN = 20.0f;          // driver default
const float AMPS_PER_COUNT = ADC_REF_VOLTS / 1024 / (SHUNT_OHMS * CSA_GAIN);
const float BUS_DIVIDER = 24.1f;       // from 0.664 V at the pin with a nominal 16 V supply

const uint8_t REG_FAULT_STATUS_1 = 0x0;
const uint8_t REG_FAULT_STATUS_2 = 0x1;
const uint8_t REG_DRIVER_CONTROL = 0x2;
const uint8_t REG_OCP_CONTROL = 0x5;
const uint16_t DRIVER_DATA_MASK = 0x07FF;
const uint16_t DRIVER_READ_FLAG = 0x8000;
const uint16_t DRIVER_FAULT_BIT = 1 << 10;
const uint16_t DRIVER_CONTROL_CLR_FLT = 1 << 0;
const uint16_t DRIVER_CONTROL_PWM_3X = 1 << 5;
// Default OCP settings (0x159) but with OCP_MODE = latched shutdown, not auto-retry:
// an overcurrent turns everything off and keeps its fault bits set until cleared.
const uint16_t OCP_CONTROL_LATCHED = 0x119;

const uint16_t ENCODER_READ_ANGLE = 0xFFFF;  // read 0x3FFF, with parity
const uint16_t ENCODER_DATA_MASK = 0x3FFF;

// Both chips: SPI mode 1, MSB first. The driver's SDO is open drain, so it gets a slow clock.
const SPISettings DRIVER_SPI(100000, MSBFIRST, SPI_MODE1);
const SPISettings ENCODER_SPI(1000000, MSBFIRST, SPI_MODE1);

float offsetB;
float offsetC;

uint16_t transfer(uint8_t csPin, const SPISettings &settings, uint16_t out) {
  SPI.beginTransaction(settings);
  digitalWrite(csPin, LOW);
  uint16_t in = SPI.transfer16(out);
  digitalWrite(csPin, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);  // both chips need chip select high >= 400 ns between words
  return in;
}

uint16_t readDriver(uint8_t reg) {
  return transfer(PIN_DRIVER_CS, DRIVER_SPI, DRIVER_READ_FLAG | ((uint16_t)reg << 11)) & DRIVER_DATA_MASK;
}

void writeDriver(uint8_t reg, uint16_t data) {
  transfer(PIN_DRIVER_CS, DRIVER_SPI, ((uint16_t)reg << 11) | (data & DRIVER_DATA_MASK));
}

// The encoder answers in the following frame, so send the command twice.
uint16_t readEncoder() {
  transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE);
  return transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE) & ENCODER_DATA_MASK;
}

// Mean of `samples` conversions, in ADC counts.
float readAveraged(uint8_t pin, uint16_t samples) {
  analogRead(pin);  // discard the first conversion after switching channel
  uint32_t sum = 0;
  for (uint16_t i = 0; i < samples; i++) sum += analogRead(pin);
  return (float)sum / samples;
}

void setPulse(uint16_t counts) {
  OCR1A = counts;
}

// All three half-bridges Hi-Z.
void outputsOff() {
  setPulse(0);
  for (uint8_t pin : PINS_INL) digitalWrite(pin, LOW);
}

// The control register reads 0x20 while the driver is still in 3x PWM mode;
// 0x0 means the driver reset itself (for example on a supply dip).
void printFaults() {
  Serial.print(F("status1 = 0x"));
  Serial.print(readDriver(REG_FAULT_STATUS_1), HEX);
  Serial.print(F("  status2 = 0x"));
  Serial.print(readDriver(REG_FAULT_STATUS_2), HEX);
  Serial.print(F("  control = 0x"));
  Serial.println(readDriver(REG_DRIVER_CONTROL), HEX);
}

// nFAULT is only pulled up to 3.3 V, close to the Uno's logic threshold, so a
// low reading counts only if the driver's own FAULT bit agrees or the driver
// has dropped out of 3x PWM mode.
bool driverFaulted() {
  if (digitalRead(PIN_NFAULT)) return false;
  if (readDriver(REG_FAULT_STATUS_1) & DRIVER_FAULT_BIT) return true;
  return readDriver(REG_DRIVER_CONTROL) != DRIVER_CONTROL_PWM_3X;
}

void halt(const __FlashStringHelper *reason) {
  outputsOff();
  Serial.print(F("STOPPED: "));
  Serial.println(reason);
  printFaults();
  digitalWrite(PIN_ENABLE, LOW);
  while (true) {}
}

void setup() {
  Serial.begin(115200);

  digitalWrite(PIN_INH_A, LOW);
  pinMode(PIN_INH_A, OUTPUT);
  digitalWrite(PIN_INH_B, LOW);
  pinMode(PIN_INH_B, OUTPUT);
  digitalWrite(PIN_INH_C, LOW);
  pinMode(PIN_INH_C, OUTPUT);
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

  // Timer1: phase-correct PWM, TOP = ICR1, no prescaler, output on OC1A (D9) only.
  ICR1 = PWM_TOP;
  OCR1A = 0;
  TCCR1A = _BV(COM1A1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(CS10);

  // Toggling ENABLE resets the driver's registers to their defaults.
  pinMode(PIN_ENABLE, OUTPUT);
  digitalWrite(PIN_ENABLE, LOW);
  delay(5);
  digitalWrite(PIN_ENABLE, HIGH);
  delay(5);
  writeDriver(REG_DRIVER_CONTROL, DRIVER_CONTROL_PWM_3X | DRIVER_CONTROL_CLR_FLT);
  writeDriver(REG_OCP_CONTROL, OCP_CONTROL_LATCHED);

  Serial.println(F("\n--- alignment test ---"));
  if (readDriver(REG_DRIVER_CONTROL) != DRIVER_CONTROL_PWM_3X) halt(F("driver not responding - check motor supply"));
  if (readDriver(REG_OCP_CONTROL) != OCP_CONTROL_LATCHED) halt(F("could not set latched overcurrent mode"));

  delay(100);
  offsetB = readAveraged(PIN_ISEN_B, 1024);
  offsetC = readAveraged(PIN_ISEN_C, 1024);
  float busVolts = readAveraged(PIN_VSEN_BUS, 64) * ADC_REF_VOLTS / 1024 * BUS_DIVIDER;

  Serial.print(F("zero offsets  B: "));
  Serial.print(offsetB, 1);
  Serial.print(F("  C: "));
  Serial.println(offsetC, 1);
  Serial.print(F("bus volts: "));
  Serial.println(busVolts, 2);
  Serial.print(F("encoder before: "));
  Serial.println(readEncoder());

  Serial.println(F("Motor connected and clamped, no propeller? Type g to start."));
  while (Serial.read() != 'g') {}

  // B and C low sides on, A low side on between pulses.
  for (uint8_t pin : PINS_INL) digitalWrite(pin, HIGH);

  Serial.println(F("pulse\tvolts\tiB\tiC\tiA"));
  uint16_t pulse = 0;
  float volts = 0;
  float ampsA = 0;
  while (true) {
    setPulse(pulse);
    delay(100);

    float ampsB = (readAveraged(PIN_ISEN_B, 64) - offsetB) * AMPS_PER_COUNT;
    float ampsC = (readAveraged(PIN_ISEN_C, 64) - offsetC) * AMPS_PER_COUNT;
    ampsA = fabs(ampsB + ampsC);
    volts = busVolts * pulse / PWM_TOP;

    Serial.print(pulse);
    Serial.print('\t');
    Serial.print(volts, 3);
    Serial.print('\t');
    Serial.print(ampsB, 2);
    Serial.print('\t');
    Serial.print(ampsC, 2);
    Serial.print('\t');
    Serial.println(ampsA, 2);

    if (driverFaulted()) halt(F("driver fault"));
    if (fabs(ampsB) > PHASE_LIMIT_AMPS || fabs(ampsC) > PHASE_LIMIT_AMPS) halt(F("phase current over limit"));
    if (ampsA >= TARGET_AMPS) break;
    if (pulse >= MAX_PULSE) break;
    pulse++;
  }

  // Let the rotor settle on electrical zero, then read where the encoder thinks it is.
  delay(1000);
  uint16_t raw = readEncoder();
  outputsOff();

  Serial.println(ampsA >= TARGET_AMPS ? F("reached target current") : F("hit the duty cap before the target current"));
  Serial.print(F("encoder at electrical zero: "));
  Serial.println(raw);
  Serial.print(F("electrical offset (counts): "));
  Serial.println((uint16_t)((uint32_t)raw * POLE_PAIRS % ENCODER_COUNTS));
  // A in series with B and C in parallel is 1.5 phase resistances.
  Serial.print(F("phase resistance estimate (ohms): "));
  Serial.println(ampsA > 0.2f ? volts / (1.5f * ampsA) : 0.0f, 4);
  printFaults();
  Serial.println(F("outputs off"));
}

void loop() {}

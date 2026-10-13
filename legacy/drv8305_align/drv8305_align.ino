// First powered test on the BOOSTXL-DRV8305EVM: push a small DC current into
// phase A and out through phases B and C, ramping slowly until it reaches
// TARGET_AMPS.
//
// This pulls the rotor to electrical angle zero, so it gives us three things:
//   - proof that the power stage and all three current sensors work
//   - an estimate of the motor's phase resistance
//   - the encoder reading at electrical zero (the alignment offset)
//
// Phase A is PWM'd with very short pulses; B and C inputs stay low, which in
// 3-PWM mode keeps their low-side MOSFETs on. Their shunts therefore carry
// continuous current, and A's shunt carries it between pulses, so all three
// can be sampled without synchronising to the PWM. iA + iB + iC should be ~0.
//
// Wiring (Uno): as drv8305_check, with the motor's three wires on MOTA/B/C.
// Type 'g' in the Serial Monitor to start.

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = 2;
const uint8_t PIN_DRIVER_CS = 6;
const uint8_t PIN_EN_GATE = 7;
const uint8_t PIN_NFAULT = 8;
const uint8_t PIN_INH_A = 9;  // OC1A
const uint8_t PIN_INH_B = 10;
const uint8_t PIN_INH_C = 3;
const uint8_t PINS_ISEN[] = {A0, A1, A2};
const uint8_t PIN_VSEN_BUS = A3;

const uint8_t POLE_PAIRS = 14;
const uint16_t ENCODER_COUNTS = 16384;

const float TARGET_AMPS = 2.0f;        // stop ramping once phase A reaches this
const float PHASE_LIMIT_AMPS = 3.0f;   // abort if any one phase exceeds this
const uint16_t MAX_PULSE = 160;        // hard cap on duty: 160 / 2000 = 8 %
const uint16_t PWM_TOP = 2000;         // 16 MHz / (2 * 2000) = 4 kHz, 125 ns per count

const float ADC_REF_VOLTS = 5.0f;      // Uno default reference (USB 5 V, so approximate)
const float SHUNT_OHMS = 0.007f;       // R4, R5, R6 on the EVM
const float CSA_GAIN = 20.0f;          // set below through the shunt amplifier register
const float AMPS_PER_COUNT = ADC_REF_VOLTS / 1024 / (SHUNT_OHMS * CSA_GAIN);
const float BUS_DIVIDER = (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM

const uint8_t REG_WARNINGS = 0x1;
const uint8_t REG_VDS_FAULTS = 0x2;
const uint8_t REG_IC_FAULTS = 0x3;
const uint8_t REG_VGS_FAULTS = 0x4;
const uint8_t REG_GATE_DRIVE = 0x7;
const uint8_t REG_SHUNT_AMP = 0xA;
const uint16_t DRIVER_DATA_MASK = 0x07FF;
const uint16_t DRIVER_READ_FLAG = 0x8000;
const uint16_t WARNINGS_FAULT_BIT = 1 << 10;
const uint16_t GATE_DRIVE_PWM_3X = 0x296;   // power-on default (0x216) with PWM_MODE = 3 inputs
const uint16_t SHUNT_AMP_GAIN_20 = 0x015;   // 20 V/V on all three amplifiers

const uint16_t ENCODER_READ_ANGLE = 0xFFFF;  // read 0x3FFF, with parity
const uint16_t ENCODER_DATA_MASK = 0x3FFF;

// Both chips: SPI mode 1, MSB first.
const SPISettings DRIVER_SPI(100000, MSBFIRST, SPI_MODE1);
const SPISettings ENCODER_SPI(1000000, MSBFIRST, SPI_MODE1);

float offsets[3];

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

float readAmps(uint8_t phase) {
  return (readAveraged(PINS_ISEN[phase], 64) - offsets[phase]) * AMPS_PER_COUNT;
}

void setPulse(uint16_t counts) {
  OCR1A = counts;
}

void printFaults() {
  Serial.print(F("warn=0x"));
  Serial.print(readDriver(REG_WARNINGS), HEX);
  Serial.print(F(" vds=0x"));
  Serial.print(readDriver(REG_VDS_FAULTS), HEX);
  Serial.print(F(" ic=0x"));
  Serial.print(readDriver(REG_IC_FAULTS), HEX);
  Serial.print(F(" vgs=0x"));
  Serial.print(readDriver(REG_VGS_FAULTS), HEX);
  Serial.print(F(" gate=0x"));
  Serial.println(readDriver(REG_GATE_DRIVE), HEX);
}

// Reads the fault registers before disabling the gate driver, because taking
// EN_GATE low clears them. EN_GATE low turns all six MOSFETs off.
void halt(const __FlashStringHelper *reason) {
  setPulse(0);
  Serial.print(F("STOPPED: "));
  Serial.println(reason);
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);
  while (true) {}
}

// nFAULT is only pulled up to 3.3 V, close to the Uno's logic threshold, so a
// low reading counts only if the driver's own FAULT bit agrees.
bool driverFaulted() {
  if (digitalRead(PIN_NFAULT)) return false;
  return readDriver(REG_WARNINGS) & WARNINGS_FAULT_BIT;
}

void setup() {
  Serial.begin(115200);

  digitalWrite(PIN_EN_GATE, LOW);
  pinMode(PIN_EN_GATE, OUTPUT);
  digitalWrite(PIN_INH_A, LOW);
  pinMode(PIN_INH_A, OUTPUT);
  digitalWrite(PIN_INH_B, LOW);
  pinMode(PIN_INH_B, OUTPUT);
  digitalWrite(PIN_INH_C, LOW);
  pinMode(PIN_INH_C, OUTPUT);

  digitalWrite(PIN_ENCODER_CS, HIGH);
  pinMode(PIN_ENCODER_CS, OUTPUT);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  pinMode(PIN_DRIVER_CS, OUTPUT);
  pinMode(PIN_NFAULT, INPUT);
  SPI.begin();
  delay(10);

  // Timer1: phase-correct PWM, TOP = ICR1, no prescaler, output on OC1A (D9) only.
  ICR1 = PWM_TOP;
  OCR1A = 0;
  TCCR1A = _BV(COM1A1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(CS10);

  Serial.println(F("\n--- DRV8305 alignment test ---"));
  writeDriver(REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);
  writeDriver(REG_SHUNT_AMP, SHUNT_AMP_GAIN_20);
  if (readDriver(REG_GATE_DRIVE) != GATE_DRIVE_PWM_3X || readDriver(REG_SHUNT_AMP) != SHUNT_AMP_GAIN_20) {
    halt(F("driver not responding - check motor supply"));
  }

  Serial.println(F("Motor connected and clamped, no propeller? Type g to start."));
  while (Serial.read() != 'g') {}

  // All inputs are low, so this turns on the three low-side MOSFETs (motor braked).
  digitalWrite(PIN_EN_GATE, HIGH);
  delay(100);
  if (driverFaulted()) halt(F("driver fault on enable"));

  for (uint8_t phase = 0; phase < 3; phase++) offsets[phase] = readAveraged(PINS_ISEN[phase], 1024);
  float busVolts = readAveraged(PIN_VSEN_BUS, 64) * ADC_REF_VOLTS / 1024 * BUS_DIVIDER;

  Serial.print(F("zero offsets  A: "));
  Serial.print(offsets[0], 1);
  Serial.print(F("  B: "));
  Serial.print(offsets[1], 1);
  Serial.print(F("  C: "));
  Serial.println(offsets[2], 1);
  Serial.print(F("bus volts: "));
  Serial.println(busVolts, 2);
  Serial.print(F("encoder before: "));
  Serial.println(readEncoder());

  Serial.println(F("pulse\tvolts\tiA\tiB\tiC\tsum"));
  uint16_t pulse = 0;
  float volts = 0;
  float ampsOut = 0;
  while (true) {
    setPulse(pulse);
    delay(100);

    float ampsA = readAmps(0);
    float ampsB = readAmps(1);
    float ampsC = readAmps(2);
    ampsOut = fabs(ampsB + ampsC);
    volts = busVolts * pulse / PWM_TOP;

    Serial.print(pulse);
    Serial.print('\t');
    Serial.print(volts, 3);
    Serial.print('\t');
    Serial.print(ampsA, 2);
    Serial.print('\t');
    Serial.print(ampsB, 2);
    Serial.print('\t');
    Serial.print(ampsC, 2);
    Serial.print('\t');
    Serial.println(ampsA + ampsB + ampsC, 2);

    if (driverFaulted()) halt(F("driver fault"));
    if (fabs(ampsA) > PHASE_LIMIT_AMPS || fabs(ampsB) > PHASE_LIMIT_AMPS || fabs(ampsC) > PHASE_LIMIT_AMPS) {
      halt(F("phase current over limit"));
    }
    if (ampsOut >= TARGET_AMPS) break;
    if (pulse >= MAX_PULSE) break;
    pulse++;
  }

  // Let the rotor settle on electrical zero, then read where the encoder thinks it is.
  delay(1000);
  uint16_t raw = readEncoder();
  setPulse(0);
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);

  Serial.println(ampsOut >= TARGET_AMPS ? F("reached target current") : F("hit the duty cap before the target current"));
  Serial.print(F("encoder at electrical zero: "));
  Serial.println(raw);
  Serial.print(F("electrical offset (counts): "));
  Serial.println((uint16_t)((uint32_t)raw * POLE_PAIRS % ENCODER_COUNTS));
  // A in series with B and C in parallel is 1.5 phase resistances.
  Serial.print(F("phase resistance estimate (ohms): "));
  Serial.println(ampsOut > 0.2f ? volts / (1.5f * ampsOut) : 0.0f, 4);
  Serial.println(F("outputs off"));
}

void loop() {}

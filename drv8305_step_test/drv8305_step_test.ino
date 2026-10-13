// Stepped open-loop diagnostic on the BOOSTXL-DRV8305EVM.
//
// Holds a small voltage vector still at 30-degree electrical steps and, at
// each step, prints the three phase currents, the encoder and the bus voltage.
// A healthy setup shows the currents following three cosines 120 degrees
// apart and the encoder advancing about 98 counts per step.
//
// It first prints the same readings with every phase at 50 % duty (no vector
// commanded), where all three currents should be near zero.
//
// PWM scheme and wiring: as drv8305_open_loop. Type 'g' to start.

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = 2;
const uint8_t PIN_DRIVER_CS = 6;
const uint8_t PIN_EN_GATE = 7;
const uint8_t PIN_NFAULT = 8;
const uint8_t PIN_INH_A = 9;   // OC1A
const uint8_t PIN_INH_B = 10;  // OC1B
const uint8_t PIN_INH_C = 3;   // OC2B
const uint8_t PINS_ISEN[] = {A0, A1, A2};
const uint8_t PIN_VSEN_BUS = A3;

const uint16_t ENCODER_COUNTS = 16384;

const float TARGET_AMPS = 1.5f;        // stop raising the voltage once phase A reaches this
const float PHASE_LIMIT_AMPS = 5.0f;   // abort if the current vector exceeds this
const float MAX_VOLTS = 0.12f;         // cap on phase voltage amplitude, whatever the current reads
const float VOLT_STEP = 0.002f;
const uint8_t STEPS_PER_TURN = 12;      // 30 electrical degrees each
const uint16_t STEP_SETTLE_MS = 400;

// Timer1 counts 0..2040..0 at 16 MHz; Timer2 counts 0..255..0 at 2 MHz. Same period (3.92 kHz).
const uint16_t PWM_TOP = 2040;
const uint16_t PWM_CENTRE = 1024;      // 50.2 %, identical to Timer2's 128 / 255
const uint8_t PWM_CENTRE_C = 128;
const int16_t PWM_MAX_SWING = 100;     // clamp on A and B, in Timer1 counts either side of centre

const float ADC_REF_VOLTS = 5.0f;      // Uno default reference (USB 5 V, so approximate)
const float SHUNT_OHMS = 0.007f;       // R4, R5, R6 on the EVM
const float CSA_GAIN = 20.0f;          // set below through the shunt amplifier register
const float AMPS_PER_COUNT = ADC_REF_VOLTS / 1024 / (SHUNT_OHMS * CSA_GAIN);
// The shunts are in the low-side legs, which conduct about half of each PWM
// period here. Unsynchronised averaged samples therefore read about half the
// phase current. Good enough for a safety monitor, not for control.
const float LOW_SIDE_ON_FRACTION = 0.5f;
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
float busVolts;

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

// Signed shortest distance from one encoder reading to the next, in counts.
int16_t encoderDelta(uint16_t from, uint16_t to) {
  const int16_t half = ENCODER_COUNTS / 2;
  int16_t delta = (int16_t)to - (int16_t)from;
  if (delta > half) delta -= ENCODER_COUNTS;
  if (delta < -half) delta += ENCODER_COUNTS;
  return delta;
}

// Mean of `samples` conversions, in ADC counts.
float readAveraged(uint8_t pin, uint16_t samples) {
  analogRead(pin);  // discard the first conversion after switching channel
  uint32_t sum = 0;
  for (uint16_t i = 0; i < samples; i++) sum += analogRead(pin);
  return (float)sum / samples;
}

// Approximate phase current while the PWM is running. Positive = out of the driver.
float readAmps(uint8_t phase) {
  return (readAveraged(PINS_ISEN[phase], 32) - offsets[phase]) * AMPS_PER_COUNT / LOW_SIDE_ON_FRACTION;
}

// Magnitude of the current vector, from all three phases.
float readCurrentMagnitude() {
  float a = readAmps(0);
  float b = readAmps(1);
  float c = readAmps(2);
  return sqrt((a * a + b * b + c * c) * (2.0f / 3.0f));
}

// Starts both timers from zero together with every phase at 50 % (no net voltage).
void startPwm() {
  GTCCR = _BV(TSM) | _BV(PSRASY) | _BV(PSRSYNC);  // hold the prescalers while configuring

  TCCR1A = _BV(COM1A1) | _BV(COM1B1) | _BV(WGM11);  // phase-correct, TOP = ICR1
  TCCR1B = _BV(WGM13);
  ICR1 = PWM_TOP;
  OCR1A = PWM_CENTRE;
  OCR1B = PWM_CENTRE;
  TCNT1 = 0;

  TCCR2A = _BV(COM2B1) | _BV(WGM20);  // phase-correct, TOP = 255
  TCCR2B = _BV(CS21);                 // clk / 8
  OCR2B = PWM_CENTRE_C;
  TCNT2 = 0;

  TCCR1B |= _BV(CS10);  // clk / 1
  GTCCR = 0;
}

// Hands the pins back to plain outputs, which are low: all three low sides on.
void stopPwm() {
  TCCR1A = 0;
  TCCR2A = 0;
}

// Applies a voltage vector of `volts` (phase amplitude) at electrical angle `theta`.
void setVector(float volts, float theta) {
  float va = volts * cos(theta);
  float vb = volts * cos(theta - TWO_PI / 3);
  float vc = volts * cos(theta + TWO_PI / 3);
  int16_t swingA = constrain((int16_t)lround((va - vc) / busVolts * PWM_TOP), -PWM_MAX_SWING, PWM_MAX_SWING);
  int16_t swingB = constrain((int16_t)lround((vb - vc) / busVolts * PWM_TOP), -PWM_MAX_SWING, PWM_MAX_SWING);
  OCR1A = PWM_CENTRE + swingA;
  OCR1B = PWM_CENTRE + swingB;
}

// Prints one row of readings and returns the encoder value it read.
// An angle of -1 marks the row taken with no vector commanded.
uint16_t printRow(int16_t angleDegrees, uint16_t previousRaw) {
  float amps[3];
  for (uint8_t phase = 0; phase < 3; phase++) {
    amps[phase] = (readAmps(phase) + readAmps(phase) + readAmps(phase) + readAmps(phase)) / 4;
  }
  uint16_t raw = readEncoder();
  float bus = readAveraged(PIN_VSEN_BUS, 32) * ADC_REF_VOLTS / 1024 * BUS_DIVIDER;

  if (angleDegrees < 0) Serial.print(F("none"));
  else Serial.print(angleDegrees);
  for (uint8_t phase = 0; phase < 3; phase++) {
    Serial.print('\t');
    Serial.print(amps[phase], 2);
  }
  Serial.print('\t');
  Serial.print(amps[0] + amps[1] + amps[2], 2);
  Serial.print('\t');
  Serial.print(raw);
  Serial.print('\t');
  Serial.print(encoderDelta(previousRaw, raw));
  Serial.print('\t');
  Serial.println(bus, 2);
  return raw;
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
  stopPwm();
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
  stopPwm();
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

  Serial.println(F("\n--- DRV8305 stepped diagnostic ---"));
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
  busVolts = readAveraged(PIN_VSEN_BUS, 64) * ADC_REF_VOLTS / 1024 * BUS_DIVIDER;
  Serial.print(F("bus volts: "));
  Serial.println(busVolts, 2);
  if (busVolts < 6.0f) halt(F("bus voltage too low"));

  startPwm();
  delay(50);
  if (driverFaulted()) halt(F("driver fault when PWM started"));

  Serial.println(F("angle\tiA\tiB\tiC\tsum\tencoder\tdelta\tbus"));
  uint16_t previousRaw = readEncoder();
  printRow(-1, previousRaw);  // every phase at 50 %: no vector commanded

  // Raise the vector at angle zero until phase A carries the target current.
  float volts = 0;
  float ampsA = 0;
  while (volts < MAX_VOLTS && ampsA < TARGET_AMPS) {
    volts += VOLT_STEP;
    setVector(volts, 0);
    delay(50);
    ampsA = fabs(readAmps(0));
    if (driverFaulted()) halt(F("driver fault during ramp"));
    if (readCurrentMagnitude() > PHASE_LIMIT_AMPS) halt(F("current over limit during ramp"));
  }
  Serial.print(F("holding "));
  Serial.print(volts, 3);
  Serial.println(F(" V"));

  for (uint8_t step = 0; step <= STEPS_PER_TURN; step++) {
    setVector(volts, step * TWO_PI / STEPS_PER_TURN);
    delay(STEP_SETTLE_MS);
    if (driverFaulted()) halt(F("driver fault"));
    if (readCurrentMagnitude() > PHASE_LIMIT_AMPS) halt(F("current over limit"));
    previousRaw = printRow(step * (360 / STEPS_PER_TURN), previousRaw);
  }

  stopPwm();
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);
  Serial.println(F("outputs off"));
}

void loop() {}

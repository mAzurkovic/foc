// Open-loop rotation on the BOOSTXL-DRV8305EVM: hold a small voltage vector,
// turn it slowly through one mechanical revolution and watch the encoder.
//
// The rotor follows the vector, so this tells us:
//   - that all six MOSFETs switch (every phase now runs at ~50 % duty)
//   - which way the encoder counts relative to the A -> B -> C phase order
//   - whether 14 pole pairs is right (14 electrical turns = 1 mechanical turn)
//
// PWM: phases A and B are on Timer1 (16-bit), phase C on Timer2 (8-bit), both
// centre-aligned at 3.9 kHz and started together. Only voltage differences
// between phases matter, so C is parked at exactly 50 % and A and B carry the
// whole vector relative to it. That gives 16-bit resolution on every
// line-to-line voltage, as long as they stay well under half the bus voltage.
//
// Wiring (Uno): as drv8305_align. Type 'g' in the Serial Monitor to start.

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
const uint8_t EXPECTED_POLE_PAIRS = 14;

const float TARGET_AMPS = 5.0f;        // stop raising the voltage once phase A reaches this
const float PHASE_LIMIT_AMPS = 9.0f;   // abort if the current vector exceeds this
const float MAX_VOLTS = 0.30f;         // cap on phase voltage amplitude, whatever the current reads
const float VOLT_STEP = 0.002f;
const float ELECTRICAL_HZ = 1.0f;      // 14 pole pairs -> about 4 rpm
const uint8_t ELECTRICAL_TURNS = EXPECTED_POLE_PAIRS;

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

  Serial.println(F("\n--- DRV8305 open-loop rotation ---"));
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
  Serial.print(F(" V, phase A about "));
  Serial.print(ampsA, 2);
  Serial.println(F(" A"));
  delay(500);

  uint16_t startRaw = readEncoder();
  Serial.print(F("encoder at electrical zero: "));
  Serial.println(startRaw);

  Serial.println(F("turn\tencoder\tdelta\tpeak amps"));
  uint16_t previousRaw = startRaw;
  int32_t totalCounts = 0;
  float peakAmps = 0;
  uint8_t turn = 0;
  uint32_t startMicros = micros();
  uint32_t lastCheckMillis = millis();
  while (turn < ELECTRICAL_TURNS) {
    float turns = (micros() - startMicros) * 1e-6f * ELECTRICAL_HZ;
    if (turns >= turn + 1) {
      turn++;
      uint16_t raw = readEncoder();
      int16_t delta = encoderDelta(previousRaw, raw);
      totalCounts += delta;
      previousRaw = raw;
      Serial.print(turn);
      Serial.print('\t');
      Serial.print(raw);
      Serial.print('\t');
      Serial.print(delta);
      Serial.print('\t');
      Serial.println(peakAmps, 2);
      peakAmps = 0;
    }
    setVector(volts, (turns - floor(turns)) * TWO_PI);

    if (millis() - lastCheckMillis >= 100) {
      lastCheckMillis = millis();
      float amps = readCurrentMagnitude();
      if (amps > peakAmps) peakAmps = amps;
      if (driverFaulted()) halt(F("driver fault"));
      if (amps > PHASE_LIMIT_AMPS) halt(F("current over limit"));
    }
  }

  // Back at electrical zero: settle and take a second alignment reading.
  setVector(volts, 0);
  delay(500);
  uint16_t endRaw = readEncoder();
  stopPwm();
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);

  float countsPerTurn = (float)totalCounts / ELECTRICAL_TURNS;
  Serial.print(F("total encoder counts: "));
  Serial.println(totalCounts);
  Serial.print(F("counts per electrical turn: "));
  Serial.print(countsPerTurn, 1);
  Serial.print(F(" (expected +/-"));
  Serial.print((float)ENCODER_COUNTS / EXPECTED_POLE_PAIRS, 1);
  Serial.println(')');
  Serial.print(F("encoder direction: "));
  Serial.println(totalCounts > 0 ? F("counts UP with A->B->C") : F("counts DOWN with A->B->C"));
  Serial.print(F("pole pairs implied: "));
  Serial.println(countsPerTurn != 0 ? ENCODER_COUNTS / fabs(countsPerTurn) : 0.0f, 2);
  Serial.print(F("encoder at electrical zero, start / end: "));
  Serial.print(startRaw);
  Serial.print(F(" / "));
  Serial.println(endRaw);
  Serial.println(F("outputs off"));
}

void loop() {}

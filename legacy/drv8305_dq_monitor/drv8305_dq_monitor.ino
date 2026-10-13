// Open-loop rotation with the full FOC measurement chain running alongside.
//
// The motor is driven exactly as in drv8305_open_loop (a voltage vector turned
// slowly, here forwards then backwards). Nothing is fed back yet. What is new:
//   - phase currents are sampled in step with the PWM, at the centre of the
//     low-side on-time, so they are true instantaneous currents
//   - the encoder angle is converted to an electrical angle
//   - the currents are transformed into the rotor frame (d and q)
//
// In open loop the rotor lines up with the current vector, so a correct chain
// shows d ~ the full current and q ~ 0. The run ends by printing the encoder
// offset that would make q average exactly zero; running both directions
// cancels the friction lag.
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
const uint8_t ADC_CHANNELS_ISEN[] = {0, 1, 2};  // A0, A1, A2
const uint8_t PIN_VSEN_BUS = A3;

const uint16_t ENCODER_COUNTS = 16384;
const uint8_t POLE_PAIRS = 14;
// Electrical encoder reading (raw * POLE_PAIRS mod ENCODER_COUNTS) when the rotor
// sits at electrical zero. First estimate from the 5 A open-loop run.
const uint16_t ENCODER_OFFSET = 11900;

const float TARGET_AMPS = 5.0f;        // stop raising the voltage once the current vector reaches this
const float LIMIT_AMPS = 9.0f;         // abort if the current vector exceeds this
const float MAX_VOLTS = 0.30f;         // cap on phase voltage amplitude, whatever the current reads
const float VOLT_STEP = 0.002f;
const float ELECTRICAL_HZ = 1.0f;      // 14 pole pairs -> about 4 rpm
const uint8_t TURNS_EACH_WAY = 7;      // electrical turns forwards, then the same backwards

// Timer1 counts 0..2040..0 at 16 MHz; Timer2 counts 0..255..0 at 2 MHz. Same period (3.92 kHz).
const uint16_t PWM_TOP = 2040;
const uint16_t PWM_CENTRE = 1024;      // 50.2 %, identical to Timer2's 128 / 255
const uint8_t PWM_CENTRE_C = 128;
const int16_t PWM_MAX_SWING = 100;     // clamp on A and B, in Timer1 counts either side of centre
const uint8_t SAMPLE_EVERY = 4;        // sample currents on every 4th PWM period (980 Hz)

const float ADC_REF_VOLTS = 5.0f;      // Uno default reference (USB 5 V, so approximate)
const float SHUNT_OHMS = 0.007f;       // R4, R5, R6 on the EVM
const float CSA_GAIN = 20.0f;          // set below through the shunt amplifier register
const float AMPS_PER_COUNT = ADC_REF_VOLTS / 1024 / (SHUNT_OHMS * CSA_GAIN);
const float BUS_DIVIDER = (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM
const float COUNTS_TO_RADIANS = TWO_PI / ENCODER_COUNTS;
const float FILTER_GAIN = 0.02f;       // low-pass on the printed d and q currents

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

volatile uint16_t sampledCounts[3];
volatile bool sampleReady;

float offsets[3];
float busVolts;

// Fires when Timer1 reaches TOP, the middle of every phase's low-side on-time.
// Three conversions at 13 us each fit well inside that window.
ISR(TIMER1_CAPT_vect) {
  static uint8_t periods;
  if (++periods < SAMPLE_EVERY) return;
  periods = 0;
  for (uint8_t phase = 0; phase < 3; phase++) {
    ADMUX = _BV(REFS0) | ADC_CHANNELS_ISEN[phase];
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC)) {}
    sampledCounts[phase] = ADC;
  }
  sampleReady = true;
}

// Blocks until the next synchronised sample and returns the three phase
// currents in amps. Positive = out of the driver, into the motor.
void readCurrents(float amps[3]) {
  uint16_t counts[3];
  while (!sampleReady) {}
  noInterrupts();
  for (uint8_t phase = 0; phase < 3; phase++) counts[phase] = sampledCounts[phase];
  sampleReady = false;
  interrupts();
  for (uint8_t phase = 0; phase < 3; phase++) amps[phase] = (counts[phase] - offsets[phase]) * AMPS_PER_COUNT;
}

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

// The encoder answers each command in the next frame, so this returns the
// angle latched by the previous call (about 1 ms old at this loop rate).
uint16_t readEncoder() {
  return transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE) & ENCODER_DATA_MASK;
}

// Rotor electrical angle in radians, 0..2*pi, zero when the rotor is aligned with phase A.
float rotorAngle(uint16_t raw) {
  uint16_t electrical = (uint32_t)raw * POLE_PAIRS % ENCODER_COUNTS;
  return ((electrical + ENCODER_COUNTS - ENCODER_OFFSET) % ENCODER_COUNTS) * COUNTS_TO_RADIANS;
}

// Starts both timers from zero together. The pins stay plain low outputs (all
// three low sides on) until connectPwm().
void startTimers() {
  GTCCR = _BV(TSM) | _BV(PSRASY) | _BV(PSRSYNC);  // hold the prescalers while configuring

  TCCR1A = _BV(WGM11);  // phase-correct, TOP = ICR1
  TCCR1B = _BV(WGM13);
  ICR1 = PWM_TOP;
  OCR1A = PWM_CENTRE;
  OCR1B = PWM_CENTRE;
  TCNT1 = 0;

  TCCR2A = _BV(WGM20);  // phase-correct, TOP = 255
  TCCR2B = _BV(CS21);   // clk / 8
  OCR2B = PWM_CENTRE_C;
  TCNT2 = 0;

  TCCR1B |= _BV(CS10);  // clk / 1
  GTCCR = 0;

  ADMUX = _BV(REFS0);
  ADCSRA = _BV(ADEN) | _BV(ADPS2);  // ADC clock 1 MHz: 13 us per conversion
  TIMSK1 = _BV(ICIE1);
}

// Hands the three pins to the timers, every phase at 50 % (no net voltage).
void connectPwm() {
  TCCR1A |= _BV(COM1A1) | _BV(COM1B1);
  TCCR2A |= _BV(COM2B1);
}

// Hands the pins back to plain outputs, which are low: all three low sides on.
void stopPwm() {
  TIMSK1 = 0;
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

float magnitude(const float amps[3]) {
  return sqrt((amps[0] * amps[0] + amps[1] * amps[1] + amps[2] * amps[2]) * (2.0f / 3.0f));
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

  Serial.println(F("\n--- DRV8305 d/q monitor ---"));
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

  busVolts = analogRead(PIN_VSEN_BUS) * ADC_REF_VOLTS / 1024 * BUS_DIVIDER;
  Serial.print(F("bus volts: "));
  Serial.println(busVolts, 2);
  if (busVolts < 6.0f) halt(F("bus voltage too low"));

  // Zero offsets, taken through the same synchronised sampling path with no switching.
  startTimers();
  float sums[3] = {0, 0, 0};
  const uint16_t offsetSamples = 512;
  for (uint16_t i = 0; i < offsetSamples; i++) {
    float counts[3];
    readCurrents(counts);  // offsets are still zero, so these are raw counts * AMPS_PER_COUNT
    for (uint8_t phase = 0; phase < 3; phase++) sums[phase] += counts[phase];
  }
  for (uint8_t phase = 0; phase < 3; phase++) offsets[phase] = sums[phase] / offsetSamples / AMPS_PER_COUNT;
  Serial.print(F("zero offsets  A: "));
  Serial.print(offsets[0], 1);
  Serial.print(F("  B: "));
  Serial.print(offsets[1], 1);
  Serial.print(F("  C: "));
  Serial.println(offsets[2], 1);

  connectPwm();
  delay(50);
  if (driverFaulted()) halt(F("driver fault when PWM started"));

  // Raise the vector at angle zero until the current vector reaches the target.
  float amps[3];
  float volts = 0;
  float current = 0;
  while (volts < MAX_VOLTS && current < TARGET_AMPS) {
    volts += VOLT_STEP;
    setVector(volts, 0);
    delay(30);
    readCurrents(amps);
    readCurrents(amps);  // the first may predate the new vector
    current = magnitude(amps);
    if (driverFaulted()) halt(F("driver fault during ramp"));
    if (current > LIMIT_AMPS) halt(F("current over limit during ramp"));
  }
  Serial.print(F("holding "));
  Serial.print(volts, 3);
  Serial.print(F(" V, "));
  Serial.print(current, 2);
  Serial.println(F(" A"));
  delay(500);
  readEncoder();  // prime the encoder's one-frame pipeline

  Serial.println(F("cmd\trotor\tid\tiq\tamps"));
  float filteredD = 0;
  float filteredQ = 0;
  float sumD[2] = {0, 0};  // forwards, backwards
  float sumQ[2] = {0, 0};
  uint32_t samples[2] = {0, 0};
  uint32_t startMicros = micros();
  uint32_t lastPrintMillis = millis();
  const float totalTurns = 2.0f * TURNS_EACH_WAY;
  while (true) {
    float elapsedTurns = (micros() - startMicros) * 1e-6f * ELECTRICAL_HZ;
    if (elapsedTurns >= totalTurns) break;
    uint8_t leg = elapsedTurns < TURNS_EACH_WAY ? 0 : 1;
    float turns = leg == 0 ? elapsedTurns : totalTurns - elapsedTurns;
    float commandAngle = (turns - floor(turns)) * TWO_PI;
    setVector(volts, commandAngle);

    readCurrents(amps);
    float theta = rotorAngle(readEncoder());
    float alpha = amps[0];
    float beta = (amps[1] - amps[2]) * 0.57735027f;  // 1 / sqrt(3)
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);
    float d = alpha * cosTheta + beta * sinTheta;
    float q = beta * cosTheta - alpha * sinTheta;

    filteredD += FILTER_GAIN * (d - filteredD);
    filteredQ += FILTER_GAIN * (q - filteredQ);
    sumD[leg] += d;
    sumQ[leg] += q;
    samples[leg]++;

    current = magnitude(amps);
    if (current > LIMIT_AMPS) halt(F("current over limit"));

    if (millis() - lastPrintMillis >= 250) {
      lastPrintMillis = millis();
      if (driverFaulted()) halt(F("driver fault"));
      Serial.print(commandAngle * RAD_TO_DEG, 0);
      Serial.print('\t');
      Serial.print(theta * RAD_TO_DEG, 0);
      Serial.print('\t');
      Serial.print(filteredD, 2);
      Serial.print('\t');
      Serial.print(filteredQ, 2);
      Serial.print('\t');
      Serial.println(current, 2);
    }
  }

  stopPwm();
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);

  // Angle of the average current vector in the rotor frame, each direction.
  float errorForwards = atan2(sumQ[0], sumD[0]);
  float errorBackwards = atan2(sumQ[1], sumD[1]);
  float error = (errorForwards + errorBackwards) / 2;
  Serial.print(F("samples per second: "));
  Serial.println((samples[0] + samples[1]) / (totalTurns / ELECTRICAL_HZ), 0);
  Serial.print(F("mean id / iq forwards:  "));
  Serial.print(sumD[0] / samples[0], 2);
  Serial.print(F(" / "));
  Serial.println(sumQ[0] / samples[0], 2);
  Serial.print(F("mean id / iq backwards: "));
  Serial.print(sumD[1] / samples[1], 2);
  Serial.print(F(" / "));
  Serial.println(sumQ[1] / samples[1], 2);
  Serial.print(F("angle error forwards / backwards (deg): "));
  Serial.print(errorForwards * RAD_TO_DEG, 1);
  Serial.print(F(" / "));
  Serial.println(errorBackwards * RAD_TO_DEG, 1);
  int32_t corrected = (int32_t)ENCODER_OFFSET - lround(error / COUNTS_TO_RADIANS);
  Serial.print(F("suggested ENCODER_OFFSET: "));
  Serial.println((uint16_t)((corrected % ENCODER_COUNTS + ENCODER_COUNTS) % ENCODER_COUNTS));
  Serial.println(F("outputs off"));
}

void loop() {}

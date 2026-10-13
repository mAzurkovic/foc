// Speed control on the BOOSTXL-DRV8305EVM: a velocity loop wrapped around the
// field-oriented current loop from drv8305_current_loop.
//
// The outer loop compares the measured speed with the target and asks the
// current loop for more or less torque (q current). It therefore pushes
// through cogging and friction on its own, without needing a tap to start.
//
// Commands, typed in the Serial Monitor (line ending: Newline):
//   v<rpm>    speed target, e.g. v60 or v-60 (limited to +/-250)
//   q<amps>   switch to plain torque control with this q current
//   s         stop: speed target zero (holds zero speed, not a position)
//   x         switch the outputs off and stop
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
// sits at electrical zero. From drv8305_dq_monitor, both directions averaged.
const uint16_t ENCODER_OFFSET = 12130;

const float TARGET_LIMIT_AMPS = 6.0f;  // largest d or q target accepted
const float LIMIT_AMPS = 9.0f;         // abort if the current vector exceeds this
const float MAX_VOLTS = 0.50f;         // cap on phase voltage amplitude: bounds top speed (about 280 rpm no-load)
const float LIMIT_RPM = 400.0f;        // abort above this speed
const float TARGET_LIMIT_RPM = 250.0f; // largest speed target accepted

// Velocity PI controller: q-current request from speed error. Starting values
// from an estimated rotor inertia; expect to tune them.
const float SPEED_KP = 0.02f;          // amps per rpm of error
const float SPEED_KI = 0.10f;          // amps per rpm of error, per second

// Integral-only current controller. The winding's time constant is shorter
// than the loop period, so between samples the motor behaves like a plain
// resistor: each step moves the voltage a fraction of (error * resistance).
const float PHASE_OHMS = 0.038f;
const float LOOP_GAIN = 0.25f;
const float VOLTS_PER_AMP_STEP = LOOP_GAIN * PHASE_OHMS;

// Timer1 counts 0..2040..0 at 16 MHz; Timer2 counts 0..255..0 at 2 MHz. Same period (3.92 kHz).
const uint16_t PWM_TOP = 2040;
const uint16_t PWM_CENTRE = 1024;      // 50.2 %, identical to Timer2's 128 / 255
const uint8_t PWM_CENTRE_C = 128;
const int16_t PWM_MAX_SWING = 170;     // clamp on A and B, in Timer1 counts either side of centre
const uint8_t SAMPLE_EVERY = 6;        // sample currents on every 6th PWM period
// The three conversions take about 40 us in total. Starting them 20 us before
// the centre of the low-side on-time puts the middle one on the centre.
// Timer2 ticks are 0.5 us.
const uint8_t SAMPLE_LEAD_TICKS = 40;
const float LOOP_HZ = 16e6f / (2.0f * PWM_TOP) / SAMPLE_EVERY;  // 654 Hz

const float ADC_REF_VOLTS = 5.0f;      // Uno default reference (USB 5 V, so approximate)
const float SHUNT_OHMS = 0.007f;       // R4, R5, R6 on the EVM
const float CSA_GAIN = 20.0f;          // set below through the shunt amplifier register
const float AMPS_PER_COUNT = ADC_REF_VOLTS / 1024 / (SHUNT_OHMS * CSA_GAIN);
const float BUS_DIVIDER = (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM
const float COUNTS_TO_RADIANS = TWO_PI / ENCODER_COUNTS;
const float FILTER_GAIN = 0.02f;       // low-pass on the printed d and q currents
const float SPEED_FILTER_GAIN = 0.1f;  // low-pass on the speed estimate
// How far ahead of the encoder reading, in loop periods, the new voltage takes
// effect. Measured: 2.0 put the no-load voltage 18 degrees off the q axis at 170 rpm.
const float VOLTAGE_LEAD_LOOPS = 1.1f;

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

bool speedMode = true;
float targetRpm;
float targetQ;

// Fires shortly before the timers reach TOP, the middle of every phase's
// low-side on-time. The winding inductance is so low that at speed the current
// swings by amps within one PWM period, so sampling off-centre reads a biased
// value. Phase-correct mode also matches on the way back down; that one is skipped.
ISR(TIMER2_COMPA_vect) {
  static uint8_t periods;
  if (TCNT2 < OCR2A) return;
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
// response to the previous call's command.
uint16_t readEncoder() {
  return transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE) & ENCODER_DATA_MASK;
}

// Rotor electrical position in encoder counts, 0..ENCODER_COUNTS-1, zero when aligned with phase A.
uint16_t electricalCounts(uint16_t raw) {
  uint16_t electrical = (uint32_t)raw * POLE_PAIRS % ENCODER_COUNTS;
  return (electrical + ENCODER_COUNTS - ENCODER_OFFSET) % ENCODER_COUNTS;
}

// Signed shortest distance between two electrical positions, in counts.
int16_t countsDelta(uint16_t from, uint16_t to) {
  const int16_t half = ENCODER_COUNTS / 2;
  int16_t delta = (int16_t)to - (int16_t)from;
  if (delta > half) delta -= ENCODER_COUNTS;
  if (delta < -half) delta += ENCODER_COUNTS;
  return delta;
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
  OCR2A = 255 - SAMPLE_LEAD_TICKS;
  TIMSK2 = _BV(OCIE2A);
}

// Hands the three pins to the timers, every phase at 50 % (no net voltage).
void connectPwm() {
  TCCR1A |= _BV(COM1A1) | _BV(COM1B1);
  TCCR2A |= _BV(COM2B1);
}

// Hands the pins back to plain outputs, which are low: all three low sides on.
void stopPwm() {
  TIMSK2 = 0;
  TCCR1A = 0;
  TCCR2A = 0;
}

// Applies a voltage vector given in the stationary frame (alpha along phase A), in volts.
void setAlphaBeta(float alpha, float beta) {
  // va - vc and vb - vc, since phase C is parked at 50 %.
  float ac = 1.5f * alpha + 0.8660254f * beta;
  float bc = 1.7320508f * beta;
  int16_t swingA = constrain((int16_t)lround(ac / busVolts * PWM_TOP), -PWM_MAX_SWING, PWM_MAX_SWING);
  int16_t swingB = constrain((int16_t)lround(bc / busVolts * PWM_TOP), -PWM_MAX_SWING, PWM_MAX_SWING);
  OCR1A = PWM_CENTRE + swingA;
  OCR1B = PWM_CENTRE + swingB;
}

// Reads one command line from the Serial Monitor without blocking. Returns
// false when the user asked to stop.
bool handleSerial() {
  static char line[12];
  static uint8_t length;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (length < sizeof(line) - 1) line[length++] = c;
      continue;
    }
    line[length] = 0;
    length = 0;
    float value = atof(line + 1);
    if (line[0] == 'v') {
      targetRpm = constrain(value, -TARGET_LIMIT_RPM, TARGET_LIMIT_RPM);
      speedMode = true;
    }
    if (line[0] == 'q') {
      targetQ = constrain(value, -TARGET_LIMIT_AMPS, TARGET_LIMIT_AMPS);
      speedMode = false;
    }
    if (line[0] == 's') {
      targetRpm = 0;
      speedMode = true;
    }
    if (line[0] == 'x') return false;
  }
  return true;
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

  Serial.println(F("\n--- DRV8305 speed control ---"));
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

  Serial.print(F("loop rate (Hz): "));
  Serial.println(LOOP_HZ, 0);
  Serial.println(F("Running. Commands: v<rpm>  q<amps>  s (stop)  x (off)"));
  Serial.println(F("rpm*\trpm\tiq*\tiq\tid\tvq"));

  const float countsPerLoopToRpm = LOOP_HZ * 60.0f / ENCODER_COUNTS / POLE_PAIRS;
  float amps[3];
  float speedIntegral = 0;  // amps
  float voltsD = 0;
  float voltsQ = 0;
  float filteredD = 0;
  float filteredQ = 0;
  float speedCounts = 0;  // electrical counts per loop, filtered
  readEncoder();          // prime the encoder's one-frame pipeline
  readCurrents(amps);
  uint16_t previousPosition = electricalCounts(readEncoder());
  uint32_t lastPrintMillis = millis();

  while (handleSerial()) {
    readCurrents(amps);
    uint16_t position = electricalCounts(readEncoder());
    speedCounts += SPEED_FILTER_GAIN * (countsDelta(previousPosition, position) - speedCounts);
    previousPosition = position;

    // The encoder reading is current, but the new voltage only takes effect
    // about a loop from now, so advance its angle by the distance the rotor travels.
    float thetaNow = position * COUNTS_TO_RADIANS;
    float thetaNext = (position + VOLTAGE_LEAD_LOOPS * speedCounts) * COUNTS_TO_RADIANS;

    float alpha = amps[0];
    float beta = (amps[1] - amps[2]) * 0.57735027f;  // 1 / sqrt(3)
    float cosTheta = cos(thetaNow);
    float sinTheta = sin(thetaNow);
    float d = alpha * cosTheta + beta * sinTheta;
    float q = beta * cosTheta - alpha * sinTheta;

    float rpm = speedCounts * countsPerLoopToRpm;
    if (speedMode) {
      float error = targetRpm - rpm;
      speedIntegral = constrain(speedIntegral + SPEED_KI * error / LOOP_HZ, -TARGET_LIMIT_AMPS, TARGET_LIMIT_AMPS);
      targetQ = constrain(SPEED_KP * error + speedIntegral, -TARGET_LIMIT_AMPS, TARGET_LIMIT_AMPS);
    } else {
      speedIntegral = targetQ;  // so switching back to speed mode starts from the present torque
    }

    voltsD += VOLTS_PER_AMP_STEP * (0 - d);
    voltsQ += VOLTS_PER_AMP_STEP * (targetQ - q);
    float volts = sqrt(voltsD * voltsD + voltsQ * voltsQ);
    if (volts > MAX_VOLTS) {
      voltsD *= MAX_VOLTS / volts;
      voltsQ *= MAX_VOLTS / volts;
    }

    cosTheta = cos(thetaNext);
    sinTheta = sin(thetaNext);
    setAlphaBeta(voltsD * cosTheta - voltsQ * sinTheta, voltsD * sinTheta + voltsQ * cosTheta);

    if (magnitude(amps) > LIMIT_AMPS) halt(F("current over limit"));
    if (fabs(rpm) > LIMIT_RPM) halt(F("speed over limit"));

    filteredD += FILTER_GAIN * (d - filteredD);
    filteredQ += FILTER_GAIN * (q - filteredQ);
    if (millis() - lastPrintMillis >= 250) {
      lastPrintMillis = millis();
      if (driverFaulted()) halt(F("driver fault"));
      Serial.print(targetRpm, 0);
      Serial.print('\t');
      Serial.print(rpm, 0);
      Serial.print('\t');
      Serial.print(targetQ, 2);
      Serial.print('\t');
      Serial.print(filteredQ, 2);
      Serial.print('\t');
      Serial.print(filteredD, 2);
      Serial.print('\t');
      Serial.println(voltsQ, 3);
    }
  }

  stopPwm();
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);
  Serial.println(F("outputs off"));
}

void loop() {}

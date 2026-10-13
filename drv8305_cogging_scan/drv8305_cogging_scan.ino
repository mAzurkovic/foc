// Cogging torque scan on the BOOSTXL-DRV8305EVM.
//
// Turns the rotor very slowly through one full shaft revolution in each
// direction, in open loop with a strong current vector, and measures the
// q-axis (torque-producing) current the whole way round. At this speed that
// current is what it takes to balance the motor's cogging torque at each
// angle, plus friction. Friction changes sign with direction and cancels.
//
// The result is reported as the size and phase of a few candidate harmonics
// of the shaft angle (cycles per revolution), which is what a feed-forward
// correction in the position and speed loops needs.
//
// Takes about two and a half minutes. PWM scheme and wiring: as
// drv8305_open_loop. Type 'g' to start.

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

// Candidate cogging harmonics, in cycles per shaft revolution. 168 is the
// fundamental for 24 slots and 28 poles; 336 its second harmonic; 24, 28 and
// 84 show up with manufacturing tolerances; 14 is once per electrical turn.
const uint16_t HARMONICS[] = {14, 24, 28, 84, 168, 336};
const uint8_t HARMONIC_COUNT = sizeof(HARMONICS) / sizeof(HARMONICS[0]);

const float TARGET_AMPS = 6.0f;        // stop raising the voltage once the current vector reaches this
const float LIMIT_AMPS = 10.0f;        // abort if the current vector exceeds this
const float MAX_VOLTS = 0.30f;         // cap on phase voltage amplitude, whatever the current reads
const float VOLT_STEP = 0.002f;
const float ELECTRICAL_HZ = 0.2f;      // 14 pole pairs -> about 0.9 rpm, slow enough to be quasi-static
const uint8_t TURNS_EACH_WAY = POLE_PAIRS;  // one full shaft revolution forwards, then backwards

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

  Serial.println(F("\n--- DRV8305 cogging scan ---"));
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

  Serial.println(F("scanning: seconds, shaft degrees, mean iq so far"));
  float sumCos[HARMONIC_COUNT];
  float sumSin[HARMONIC_COUNT];
  for (uint8_t h = 0; h < HARMONIC_COUNT; h++) sumCos[h] = sumSin[h] = 0;
  float sumQ = 0;
  float sumQSquared = 0;
  float sumD = 0;
  uint32_t samples = 0;
  uint32_t startMicros = micros();
  uint32_t lastPrintMillis = millis();
  const float totalTurns = 2.0f * TURNS_EACH_WAY;
  while (true) {
    float elapsedTurns = (micros() - startMicros) * 1e-6f * ELECTRICAL_HZ;
    if (elapsedTurns >= totalTurns) break;
    float turns = elapsedTurns < TURNS_EACH_WAY ? elapsedTurns : totalTurns - elapsedTurns;
    setVector(volts, (turns - floor(turns)) * TWO_PI);

    readCurrents(amps);
    uint16_t raw = readEncoder();
    float theta = rotorAngle(raw);
    float alpha = amps[0];
    float beta = (amps[1] - amps[2]) * 0.57735027f;  // 1 / sqrt(3)
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);
    float d = alpha * cosTheta + beta * sinTheta;
    float q = beta * cosTheta - alpha * sinTheta;

    sumD += d;
    sumQ += q;
    sumQSquared += q * q;
    samples++;
    for (uint8_t h = 0; h < HARMONIC_COUNT; h++) {
      // Harmonic phase from the absolute shaft angle, wrapped in integers to keep it exact.
      float phase = (uint16_t)((uint32_t)raw * HARMONICS[h] % ENCODER_COUNTS) * COUNTS_TO_RADIANS;
      sumCos[h] += q * cos(phase);
      sumSin[h] += q * sin(phase);
    }

    current = magnitude(amps);
    if (current > LIMIT_AMPS) halt(F("current over limit"));

    if (millis() - lastPrintMillis >= 5000) {
      lastPrintMillis = millis();
      if (driverFaulted()) halt(F("driver fault"));
      Serial.print((micros() - startMicros) / 1000000UL);
      Serial.print('\t');
      Serial.print(raw * (360.0f / ENCODER_COUNTS), 0);
      Serial.print('\t');
      Serial.println(sumQ / samples, 2);
    }
  }

  stopPwm();
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);

  float meanQ = sumQ / samples;
  float variance = sumQSquared / samples - meanQ * meanQ;
  Serial.print(F("samples: "));
  Serial.println(samples);
  Serial.print(F("mean id / iq: "));
  Serial.print(sumD / samples, 2);
  Serial.print(F(" / "));
  Serial.println(meanQ, 2);
  Serial.print(F("iq ripple, RMS amps: "));
  Serial.println(sqrt(variance), 3);

  Serial.println(F("harmonic\tamps\tcos\tsin"));
  float explained = 0;
  for (uint8_t h = 0; h < HARMONIC_COUNT; h++) {
    float c = 2 * sumCos[h] / samples;
    float sn = 2 * sumSin[h] / samples;
    explained += (c * c + sn * sn) / 2;
    Serial.print(HARMONICS[h]);
    Serial.print('\t');
    Serial.print(sqrt(c * c + sn * sn), 3);
    Serial.print('\t');
    Serial.print(c, 3);
    Serial.print('\t');
    Serial.println(sn, 3);
  }
  Serial.print(F("ripple left after removing these, RMS amps: "));
  Serial.println(sqrt(max(variance - explained, 0.0f)), 3);
  Serial.println(F("outputs off"));
}

void loop() {}

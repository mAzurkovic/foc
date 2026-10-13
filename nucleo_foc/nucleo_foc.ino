// Field-oriented control on the NUCLEO-F446RE with the BOOSTXL-DRV8305EVM and
// an AS5048A encoder: current, speed and position loops.
//
// Everything time-critical runs in the ADC interrupt, once per 20 kHz PWM
// period, straight after the three phase currents are sampled:
//   every period (50 us): read the encoder, transform the currents into the
//                         rotor frame, run the d and q current controllers,
//                         set the three duty cycles
//                         track the shaft speed, guard the bus voltage
//   every 5th (250 us):   position and speed loops, cogging and friction
//                         feed-forward, braking-energy management
// The main loop only handles the Serial Monitor.
//
// Commands (line ending: Newline):
//   a           re-measure the encoder offset and direction (needed after
//               moving the motor wires or the encoder magnet)
//   d<amps>     d-axis current, no torque         q<amps>   q-axis current (torque)
//   v<rpm>      speed control                     p<deg>    go to an angle and hold
//   s           hold the present position         x         outputs off and stop
//   c0 / c1     cogging and friction feed-forward off / on
//   P K I       position gain, speed gain, speed integral gain
//   B<hz>       current-loop bandwidth            D<volts>  dead-time compensation
//   t           measure how fast the motor accelerates per amp, then set
//               P, K and I from that (the shaft spins up to a few hundred rpm)
//   W<hz>       redo that calculation for a different speed-loop bandwidth
//   R<ratio>    how many times slower each outer loop is than the one inside it
//   O<hz>       speed observer bandwidth (lower = quieter, slower to react)
//   m<rows>     status rows per second, 0 to 20 (m0 = silent, starts at 5)
// B and O are refused below 5 times the speed-loop bandwidth, and W above a
// fifth of either: a speed loop that outruns what it relies on goes unstable.
//
// Wiring: as nucleo_check, with the motor on MOTA/B/C. Type 'g' to start.

#include <math.h>

// ---------------------------------------------------------------- pins
const uint8_t PIN_ENCODER_CS = D10;  // PB6
const uint8_t PIN_DRIVER_CS = D9;    // PC7
const uint8_t PIN_EN_GATE = D6;
const uint8_t PIN_NFAULT = D5;
// PWM on PA8, PA9, PA10 (D7, D8, D2) = TIM1 CH1..3; SPI1 on PA5, PA6, PA7
// (D13, D12, D11); current sense on PA0, PA1, PA4 (A0..A2) = ADC1 channels
// 0, 1, 4; bus voltage on PB0 (A3) = channel 8.
const uint32_t ADC_CHANNEL_A = 0;
const uint32_t ADC_CHANNEL_B = 1;
const uint32_t ADC_CHANNEL_C = 4;
const uint32_t ADC_CHANNEL_BUS = 8;

// ---------------------------------------------------------------- motor and sensing
const uint8_t POLE_PAIRS = 14;
const int32_t ENCODER_COUNTS = 16384;
const float COUNTS_TO_RADIANS = TWO_PI / ENCODER_COUNTS;
const float COUNTS_TO_DEGREES = 360.0f / ENCODER_COUNTS;
// Measured with nucleo_pwm_test: resistance of the whole phase path (winding,
// MOSFET, shunt) and the winding inductance.
const float PHASE_OHMS = 0.030f;
const float PHASE_HENRIES = 5.5e-6f;

const float ADC_REF_VOLTS = 3.3f;
const float ADC_COUNTS = 4096.0f;      // 12-bit
const float SHUNT_OHMS = 0.007f;       // R4, R5, R6 on the EVM
const float CSA_GAIN = 10.0f;          // set below through the shunt amplifier register; measures to about +/-23 A
const float AMPS_PER_COUNT = ADC_REF_VOLTS / ADC_COUNTS / (SHUNT_OHMS * CSA_GAIN);
const float BUS_VOLTS_PER_COUNT = ADC_REF_VOLTS / ADC_COUNTS * (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM

// ---------------------------------------------------------------- timing
const float PWM_HZ = 20000.0f;
const float CONTROL_DT = 1.0f / PWM_HZ;
const uint8_t OUTER_DIVIDER = 5;       // outer loops run at 4 kHz
const float OUTER_HZ = PWM_HZ / OUTER_DIVIDER;
// ADC clock is PCLK2 / 4 = 22.5 MHz and each conversion takes 28 + 12 cycles
// (1.8 us). Starting the three conversions 2.7 us before the timer's peak puts
// the middle one on the peak.
const float SAMPLE_LEAD_SECONDS = 2.7e-6f;
// Time between the rotor being where the encoder says and the resulting
// voltage acting on the motor: the sensor's own latency (about 100 us) plus
// one and a half PWM periods. The angle is advanced by the distance the rotor
// travels in that time, which only matters at speed. An estimate, not yet measured.
const float ANGLE_DELAY_SECONDS = 175e-6f;
const float RPM_TO_ELECTRICAL_RADIANS_PER_SECOND = TWO_PI / 60.0f * POLE_PAIRS;

// ---------------------------------------------------------------- limits
const float TARGET_LIMIT_AMPS = 12.0f; // largest d or q target
const float TRIP_AMPS = 18.0f;         // any phase beyond this switches the outputs off at once
const float MAX_VOLTS = 2.0f;          // cap on phase voltage amplitude: bounds top speed (about 1100 rpm no-load)
const float TARGET_LIMIT_RPM = 1000.0f;
const float POSITION_LIMIT_RPM = 150.0f;
const float TRIP_RPM = 1300.0f;
// A decelerating motor pushes energy back into the supply. The mains adapter
// cannot absorb it and shuts down if its output is pushed up, so braking
// energy is burned in the motor windings instead, as d-axis current, which
// heats the copper without producing torque:
//   - feed-forward: enough d current to cancel the power the commanded braking
//     torque would return, with braking torque capped at what that allows
//   - feedback: extra d current as the bus rises above its normal level
//   - if the bus still rises, braking torque is faded out, and above the trip
//     level everything switches off
// Back-EMF per rpm: measured 0.52 V at 300 rpm (vq minus the resistive drop), rounded up.
const float BEMF_VOLTS_PER_RPM = 0.0018f;
// Returned power below this is left alone: the board's own consumption absorbs
// it. Without the allowance, the cogging correction dipping briefly negative at
// steady speed counts as braking and wastes current.
const float REGEN_ALLOWANCE_WATTS = 1.0f;
const float BUS_DUMP_START_VOLTS = 12.9f;
const float BUS_DUMP_AMPS_PER_VOLT = 15.0f;
const float BUS_BRAKE_FADE_START_VOLTS = 13.6f;
const float BUS_BRAKE_FADE_END_VOLTS = 14.2f;
const float BUS_TRIP_VOLTS = 16.0f;
const float BUS_MIN_VOLTS = 6.0f;

// ---------------------------------------------------------------- controller defaults
const float DEFAULT_CURRENT_BANDWIDTH_HZ = 500.0f;
// Voltage lost per phase to MOSFET switching time, added back in the direction
// of each phase current. Measured about 0.09 V with this gate drive; start a little under.
const float DEFAULT_DEADTIME_VOLTS = 0.07f;
const float DEADTIME_KNEE_AMPS = 1.0f;   // below this the correction scales with current
// Speed comes from a tracking observer on the encoder angle, updated every
// PWM period: a model of the shaft (position and speed) nudged towards each new
// reading. It averages the encoder's noise over many samples, where simply
// differencing two readings would amplify it.
// A higher observer bandwidth follows speed changes faster and passes more
// encoder noise into the torque.
const float DEFAULT_OBSERVER_BANDWIDTH_HZ = 250.0f;
// The speed loop must stay at least this many times slower than both the
// current loop and the speed observer.
const float MINIMUM_INNER_RATIO = 5.0f;
const float FRICTION_AMPS = 0.5f;        // feed-forward in the direction of requested motion

// Cogging feed-forward: q current that balances the cogging torque, as
// harmonics of the absolute shaft angle. Measured with drv8305_cogging_scan on
// the Uno; tied to the encoder magnet's mounting.
struct CoggingHarmonic {
  uint16_t cyclesPerTurn;
  float cosAmps;
  float sinAmps;
};
const CoggingHarmonic COGGING[] = {
  {24, -1.235f, 0.896f},
  {28, 0.251f, 0.109f},
  {84, -0.281f, 0.035f},
  {168, -0.512f, 0.026f},
};

// Tuning test: two torque levels in the same direction, so friction cancels
// and the motor never has to brake into the supply. The higher level comes
// first, so the rotor is already moving for both measurements; below about 2 A
// it does not reliably break away from rest.
const float TUNE_HIGH_AMPS = 4.5f;
const float TUNE_LOW_AMPS = 2.5f;
const uint32_t TUNE_SETTLE_MILLIS = 20;
const uint32_t TUNE_WINDOW_MILLIS = 30;
const float DEFAULT_SPEED_BANDWIDTH_HZ = 50.0f;  // no overshoot on position moves with a bare shaft
// Each outer loop is this many times slower than the loop inside it: the
// speed loop's integral relative to its bandwidth, and the position loop
// relative to the speed loop. Closer spacing makes a position move overshoot.
const float DEFAULT_LOOP_SPACING = 10.0f;
// Rpm per second per amp of q current with a bare shaft: three runs of the
// 't' command gave 3084, 3010 and 2754. It changes with anything attached to
// the shaft.
const float DEFAULT_ACCELERATION_PER_AMP = 2900.0f;

// Encoder calibration sweep: current-controlled open loop.
const float CALIBRATION_AMPS = 6.0f;
const float CALIBRATION_ELECTRICAL_HZ = 1.0f;
const uint8_t CALIBRATION_TURNS_EACH_WAY = 7;

const uint16_t OFFSET_SAMPLES = 4000;

// ---------------------------------------------------------------- DRV8305 and AS5048A
const uint8_t REG_WARNINGS = 0x1;
const uint8_t REG_VDS_FAULTS = 0x2;
const uint8_t REG_IC_FAULTS = 0x3;
const uint8_t REG_VGS_FAULTS = 0x4;
const uint8_t REG_HS_GATE_DRIVE = 0x5;
const uint8_t REG_LS_GATE_DRIVE = 0x6;
const uint8_t REG_GATE_DRIVE = 0x7;
const uint8_t REG_SHUNT_AMP = 0xA;
const uint16_t DRIVER_DATA_MASK = 0x07FF;
const uint16_t DRIVER_READ_FLAG = 0x8000;
const uint16_t WARNINGS_FAULT_BIT = 1 << 10;
const uint16_t GATE_DRIVE_PWM_3X = 0x296;    // power-on default (0x216) with PWM_MODE = 3 inputs
const uint16_t SHUNT_AMP_GAIN_10 = 0x000;    // 10 V/V on all three amplifiers
const uint16_t GATE_DRIVE_STRENGTH = 0x377;  // 0.125 A source, 0.25 A sink (default is 50 / 60 mA)

const uint16_t ENCODER_READ_ANGLE = 0xFFFF;  // read 0x3FFF, with parity
const uint16_t ENCODER_CLEAR_ERROR = 0x4001; // read 0x0001, which clears the error flag
const uint16_t ENCODER_MAX_BAD_READS = 40;   // consecutive (2 ms) before giving up
const uint16_t ENCODER_DATA_MASK = 0x3FFF;
const uint16_t ENCODER_ERROR_FLAG = 0x4000;
const int32_t ENCODER_MAX_JUMP = 400;        // counts per 50 us; more than this is a bad reading

// ---------------------------------------------------------------- state shared with the interrupt
enum Mode : uint8_t { OFFSETS, IDLE, FORCED, TORQUE, SPEED, POSITION, TRIPPED };
enum Trip : uint8_t { NO_TRIP, TRIP_CURRENT, TRIP_SPEED, TRIP_BUS_HIGH, TRIP_BUS_LOW, TRIP_ENCODER, TRIP_DRIVER, TRIP_USER };

volatile Mode mode = OFFSETS;
volatile Trip tripReason = NO_TRIP;

uint32_t pwmTop;  // timer counts from zero to peak
float sineTable[256];

// Encoder calibration, from the 'a' command. These stored values hold only
// while the motor wires stay on the same terminals and the encoder magnet is
// not remounted; run 'a' again after either change.
volatile int8_t direction = 1;          // +1 if the encoder counts up with the A -> B -> C phase order
volatile int32_t encoderOffset = 1090;  // electrical counts at electrical zero
volatile bool encoderCalibrated = true;

// Targets and gains, set from the main loop.
volatile float targetD, targetQ, targetRpm, targetDegrees;
volatile float forcedTheta, forcedStep;  // open-loop angle and its increment per period, for FORCED mode
volatile bool feedForwardEnabled = true;
volatile float positionKp, speedKp, speedKi;  // set by applyTuning()
volatile float observerPositionGain, observerSpeedGain;  // set by setObserverBandwidth()
volatile float currentKp, currentKiDt;
volatile float deadtimeVolts = DEFAULT_DEADTIME_VOLTS;

// Measurements, read by the main loop.
volatile float busVolts = 12.0f;
volatile float measuredD, measuredQ;   // low-pass filtered, for printing
volatile float commandedQ;             // q target after the outer loops and feed-forward
volatile float commandedQFiltered;     // the same, low-pass filtered for printing
volatile float rpm, shaftDegrees;
volatile uint32_t encoderErrors;
volatile float measuredVoltsQ;         // low-pass filtered q-axis voltage, for working out the back-EMF
volatile uint32_t dumpTicks;           // outer-loop ticks on which braking energy was being burned in the windings
volatile uint16_t tripRegisters[4];    // DRV8305 status registers, captured at the moment of a trip
float accelerationPerAmp = DEFAULT_ACCELERATION_PER_AMP;
float speedBandwidthHz = DEFAULT_SPEED_BANDWIDTH_HZ;
float loopSpacing = DEFAULT_LOOP_SPACING;
float currentBandwidthHz = DEFAULT_CURRENT_BANDWIDTH_HZ;
float observerBandwidthHz = DEFAULT_OBSERVER_BANDWIDTH_HZ;
uint32_t statusIntervalMillis = 200;   // 0 = no status rows
volatile uint32_t maxInterruptCycles;

// Offset measurement.
float offsets[3];
volatile uint32_t offsetCount;
volatile uint32_t offsetSum[3];

// Encoder calibration sums: the angle between the encoder's electrical
// reading and the forced angle, for both possible directions.
volatile float calibrationSin[2], calibrationCos[2];
volatile int32_t calibrationTravel;

// ---------------------------------------------------------------- low-level helpers
// Timer outputs released: the driver's pull-downs then hold every input low,
// which turns on the three low-side MOSFETs.
static inline void outputsOff() {
  TIM1->BDTR &= ~TIM_BDTR_MOE;
}

// One 16-bit SPI frame, mode 1. Every wait is bounded so a wiring fault can
// never hang the control interrupt.
static inline uint16_t spiTransfer(GPIO_TypeDef *port, uint32_t pinMask, uint16_t out) {
  port->BSRR = pinMask << 16;                // chip select low
  for (volatile uint8_t i = 0; i < 16; i++) {}  // both chips need about 0.4 us before the first clock
  SPI1->DR = out;
  for (uint32_t i = 0; i < 4000 && !(SPI1->SR & SPI_SR_RXNE); i++) {}
  uint16_t in = SPI1->DR;
  for (uint32_t i = 0; i < 4000 && (SPI1->SR & SPI_SR_BSY); i++) {}
  port->BSRR = pinMask;                      // chip select high
  return in;
}

// Stops driving the motor and lets it coast: outputs released, the driver's
// status captured, then the gate driver disabled so all six MOSFETs are off.
// Coasting is the safe state here: at these speeds the motor's back-EMF is far
// below the bus voltage, so no current flows, whereas leaving the low sides on
// would short a spinning motor through the shunts.
static inline void trip(Trip reason) {
  if (mode == TRIPPED) return;
  outputsOff();
  for (uint8_t i = 0; i < 4; i++) {
    tripRegisters[i] = spiTransfer(GPIOC, GPIO_PIN_7, DRIVER_READ_FLAG | ((uint16_t)(REG_WARNINGS + i) << 11)) & DRIVER_DATA_MASK;
  }
  GPIOB->BSRR = (uint32_t)GPIO_PIN_10 << 16;  // EN_GATE (D6) low
  tripReason = reason;
  mode = TRIPPED;
}

static inline uint16_t encoderTransfer(uint16_t command = ENCODER_READ_ANGLE) {
  return spiTransfer(GPIOB, GPIO_PIN_6, command);
}

// Driver access is only allowed while the interrupt is not using the bus
// (before the control starts, or after a trip).
uint16_t readDriver(uint8_t reg) {
  uint16_t in = spiTransfer(GPIOC, GPIO_PIN_7, DRIVER_READ_FLAG | ((uint16_t)reg << 11));
  delayMicroseconds(2);
  return in & DRIVER_DATA_MASK;
}

void writeDriver(uint8_t reg, uint16_t data) {
  spiTransfer(GPIOC, GPIO_PIN_7, ((uint16_t)reg << 11) | (data & DRIVER_DATA_MASK));
  delayMicroseconds(2);
}

static inline float tableSine(uint32_t index) {
  return sineTable[index & 255];
}

// Feed-forward q current for a raw encoder reading, before applying the direction sign.
static inline float coggingAmps(uint16_t raw) {
  float sum = 0;
  for (const CoggingHarmonic &harmonic : COGGING) {
    uint32_t index = ((uint32_t)raw * harmonic.cyclesPerTurn % ENCODER_COUNTS) >> 6;
    sum += harmonic.cosAmps * tableSine(index + 64) + harmonic.sinAmps * tableSine(index);
  }
  return sum;
}

static inline float clampf(float value, float limit) {
  return value > limit ? limit : (value < -limit ? -limit : value);
}

// Signed shortest distance between two encoder readings, in counts.
static inline int32_t wrapCounts(int32_t delta) {
  if (delta > ENCODER_COUNTS / 2) delta -= ENCODER_COUNTS;
  if (delta < -ENCODER_COUNTS / 2) delta += ENCODER_COUNTS;
  return delta;
}

// ---------------------------------------------------------------- control interrupt
// Runs once per PWM period, just after the three currents have been sampled
// at the middle of the low-side on-time.
extern "C" void ADC_IRQHandler(void) {
  if (!(ADC1->SR & ADC_SR_JEOC)) return;
  ADC1->SR = ~ADC_SR_JEOC;
  uint32_t startCycles = DWT->CYCCNT;
  uint32_t counts[3] = {ADC1->JDR1, ADC1->JDR2, ADC1->JDR3};

  static uint16_t lastRaw;
  static int32_t shaftCounts;      // multi-turn, in the encoder's own direction
  static int32_t observedCounts;   // observer position: whole counts ...
  static float observedFraction;   // ... plus a fraction
  static float observedSpeed;      // counts per second
  static uint8_t outerCountdown;
  static float integralD, integralQ, speedIntegral;
  static bool encoderPrimed;
  static bool clearEncoderError;
  static uint8_t encoderSkip;      // responses still to ignore after clearing the sensor's error flag
  static uint16_t badReads;
  static float busQuick = 12.0f;   // lightly filtered bus voltage, for the braking logic
  static float dumpFeedForward;    // d current needed to absorb the commanded braking power

  if (mode == OFFSETS) {
    for (uint8_t phase = 0; phase < 3; phase++) offsetSum[phase] += counts[phase];
    offsetCount++;
    return;
  }
  if (mode == TRIPPED) return;

  // Bus voltage: read the conversion started last period and start the next.
  float busNow = (float)ADC1->DR * BUS_VOLTS_PER_COUNT;
  ADC1->CR2 |= ADC_CR2_SWSTART;
  busQuick += 0.3f * (busNow - busQuick);
  busVolts += 0.01f * (busNow - busVolts);
  if (busQuick > BUS_TRIP_VOLTS) {
    trip(TRIP_BUS_HIGH);
    return;
  }

  // Phase currents. Positive = out of the driver, into the motor.
  float amps[3];
  for (uint8_t phase = 0; phase < 3; phase++) {
    amps[phase] = ((float)counts[phase] - offsets[phase]) * AMPS_PER_COUNT;
    if (fabsf(amps[phase]) > TRIP_AMPS) {
      trip(TRIP_CURRENT);
      return;
    }
  }

  // Encoder. A reading with bad parity, the sensor's error flag, or an
  // impossible jump is dropped and the previous angle kept. The sensor answers
  // each command in the following frame, so after a clear-error command the
  // next response is the error register, not an angle.
  bool sendingClear = clearEncoderError;
  uint16_t word = encoderTransfer(sendingClear ? ENCODER_CLEAR_ERROR : ENCODER_READ_ANGLE);
  uint16_t raw = word & ENCODER_DATA_MASK;
  bool parityOk = !__builtin_parity(word);
  bool valid = parityOk && !(word & ENCODER_ERROR_FLAG);
  if (sendingClear) {
    clearEncoderError = false;
    encoderSkip = 1;
    valid = false;
  } else if (encoderSkip > 0) {
    encoderSkip--;
    valid = false;
  } else if (parityOk && (word & ENCODER_ERROR_FLAG)) {
    clearEncoderError = true;
  }
  if (valid && encoderPrimed) {
    int32_t delta = wrapCounts((int32_t)raw - lastRaw);
    if (delta > ENCODER_MAX_JUMP || delta < -ENCODER_MAX_JUMP) valid = false;
    else shaftCounts += delta;
  }
  if (valid) {
    lastRaw = raw;
    encoderPrimed = true;
    badReads = 0;
  } else {
    encoderErrors++;
    raw = lastRaw;
    if (++badReads > ENCODER_MAX_BAD_READS) {
      trip(TRIP_ENCODER);
      return;
    }
  }

  // Speed observer: predict, compare with the encoder, correct. Speed and
  // position are reported in the electrical-positive direction.
  observedFraction += observedSpeed * CONTROL_DT;
  float observerError = (float)(shaftCounts - observedCounts) - observedFraction;
  observedFraction += observerPositionGain * observerError;
  observedSpeed += observerSpeedGain * observerError;
  int32_t whole = (int32_t)floorf(observedFraction);
  observedCounts += whole;
  observedFraction -= whole;
  rpm = direction * observedSpeed * (60.0f / ENCODER_COUNTS);

  // Outer loops, 4 kHz.
  if (++outerCountdown >= OUTER_DIVIDER) {
    outerCountdown = 0;

    if (busVolts < BUS_MIN_VOLTS) {
      trip(TRIP_BUS_LOW);
      return;
    }

    shaftDegrees = direction * shaftCounts * COUNTS_TO_DEGREES;
    if (fabsf(rpm) > TRIP_RPM) {
      trip(TRIP_SPEED);
      return;
    }

    if (mode == FORCED) {
      // Encoder electrical reading minus the forced angle, for both direction hypotheses.
      float electrical = (float)((uint32_t)raw * POLE_PAIRS % ENCODER_COUNTS) * COUNTS_TO_RADIANS;
      float forwards = electrical - forcedTheta;
      float backwards = -electrical - forcedTheta;
      calibrationSin[0] += sinf(forwards);
      calibrationCos[0] += cosf(forwards);
      calibrationSin[1] += sinf(backwards);
      calibrationCos[1] += cosf(backwards);
    }

    if (mode == POSITION) {
      targetRpm = clampf(positionKp * (targetDegrees - shaftDegrees), POSITION_LIMIT_RPM);
    }
    float q = targetQ;
    if (mode == SPEED || mode == POSITION) {
      float error = targetRpm - rpm;
      // Anti-windup: integrate only while the output is inside its limit, or
      // when doing so brings it back inside.
      float unclamped = speedKp * error + speedIntegral;
      if (fabsf(unclamped) < TARGET_LIMIT_AMPS || unclamped * error < 0) speedIntegral += speedKi * error / OUTER_HZ;
      q = clampf(speedKp * error + speedIntegral, TARGET_LIMIT_AMPS);
      if (feedForwardEnabled) q += direction * coggingAmps(raw) + FRICTION_AMPS * clampf(targetRpm, 1.0f);
    } else {
      speedIntegral = 0;
      if (mode == TORQUE && feedForwardEnabled) q += direction * coggingAmps(raw);
    }
    q = clampf(q, TARGET_LIMIT_AMPS);
    // Braking returns power 1.5 * (|q| * bemf - R * q^2) to the bus. Cap the
    // braking torque at what the windings can absorb within the current limit,
    // and work out the d current that absorbs the excess over the allowance:
    // 1.5 * R * d^2 = 1.5 * (|q| * bemf - R * q^2) - allowance.
    dumpFeedForward = 0;
    if (q * rpm < 0 && mode != FORCED) {
      float bemf = fabsf(rpm) * BEMF_VOLTS_PER_RPM;
      float mostBraking = PHASE_OHMS * TARGET_LIMIT_AMPS * TARGET_LIMIT_AMPS / (bemf + 1e-6f);
      q = clampf(q, mostBraking);
      float needSquared = (fabsf(q) * bemf - REGEN_ALLOWANCE_WATTS / 1.5f) / PHASE_OHMS - q * q;
      if (needSquared > 0) dumpFeedForward = sqrtf(needSquared);
    }
    if (dumpFeedForward > 0.5f || busQuick > BUS_DUMP_START_VOLTS) dumpTicks++;
    commandedQ = q;
    commandedQFiltered += 0.005f * (q - commandedQFiltered);
  }

  if (mode == IDLE) {
    integralD = integralQ = 0;
    TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = pwmTop / 2;
  } else {
    // Rotor electrical angle, or the forced angle during calibration.
    float theta;
    float wantD, wantQ;
    if (mode == FORCED) {
      forcedTheta += forcedStep;
      if (forcedTheta > TWO_PI) forcedTheta -= TWO_PI;
      if (forcedTheta < 0) forcedTheta += TWO_PI;
      theta = forcedTheta;
      wantD = targetD;
      wantQ = 0;
    } else {
      int32_t electrical = (int32_t)((uint32_t)raw * POLE_PAIRS % ENCODER_COUNTS);
      if (direction < 0) electrical = ENCODER_COUNTS - electrical;
      electrical = (electrical - encoderOffset) & (ENCODER_COUNTS - 1);
      theta = electrical * COUNTS_TO_RADIANS + rpm * (RPM_TO_ELECTRICAL_RADIANS_PER_SECOND * ANGLE_DELAY_SECONDS);
      wantQ = commandedQ;
      float over = busQuick - BUS_DUMP_START_VOLTS;
      float dump = dumpFeedForward + (over > 0 ? BUS_DUMP_AMPS_PER_VOLT * over : 0);
      float room = TARGET_LIMIT_AMPS * TARGET_LIMIT_AMPS - wantQ * wantQ;
      room = room > 0 ? sqrtf(room) : 0;
      if (dump > room) dump = room;
      wantD = targetD - dump;
      // Last resort: fade the braking torque out if the bus keeps rising anyway.
      if (wantQ * rpm < 0 && busQuick > BUS_BRAKE_FADE_START_VOLTS) {
        float keep = (BUS_BRAKE_FADE_END_VOLTS - busQuick) / (BUS_BRAKE_FADE_END_VOLTS - BUS_BRAKE_FADE_START_VOLTS);
        wantQ *= keep < 0 ? 0 : keep;
      }
    }
    float sinTheta = sinf(theta);
    float cosTheta = cosf(theta);

    // Currents into the rotor frame.
    float alpha = (2.0f * amps[0] - amps[1] - amps[2]) * (1.0f / 3.0f);
    float beta = (amps[1] - amps[2]) * 0.57735027f;  // 1 / sqrt(3)
    float d = alpha * cosTheta + beta * sinTheta;
    float q = beta * cosTheta - alpha * sinTheta;
    measuredD += 0.002f * (d - measuredD);
    measuredQ += 0.002f * (q - measuredQ);

    // PI current controllers, with the integrators frozen while the voltage is limited.
    float errorD = wantD - d;
    float errorQ = wantQ - q;
    float voltsD = currentKp * errorD + integralD;
    float voltsQ = currentKp * errorQ + integralQ;
    float magnitude = sqrtf(voltsD * voltsD + voltsQ * voltsQ);
    if (magnitude > MAX_VOLTS) {
      voltsD *= MAX_VOLTS / magnitude;
      voltsQ *= MAX_VOLTS / magnitude;
    } else {
      integralD += currentKiDt * errorD;
      integralQ += currentKiDt * errorQ;
    }
    measuredVoltsQ += 0.002f * (voltsQ - measuredVoltsQ);

    // Back to the stationary frame and the three phases.
    float voltsAlpha = voltsD * cosTheta - voltsQ * sinTheta;
    float voltsBeta = voltsD * sinTheta + voltsQ * cosTheta;
    float volts[3] = {
      voltsAlpha,
      -0.5f * voltsAlpha + 0.8660254f * voltsBeta,
      -0.5f * voltsAlpha - 0.8660254f * voltsBeta,
    };

    float half = 0.5f * pwmTop;
    float perVolt = pwmTop / busVolts;
    float knee = deadtimeVolts / DEADTIME_KNEE_AMPS;
    uint32_t compare[3];
    for (uint8_t phase = 0; phase < 3; phase++) {
      volts[phase] += clampf(knee * amps[phase], deadtimeVolts);  // dead-time compensation
      float value = half + volts[phase] * perVolt;
      // Keep the low-side on-time long enough for the current sample.
      float lowest = 0.1f * pwmTop;
      float highest = 0.8f * pwmTop;
      compare[phase] = (uint32_t)(value < lowest ? lowest : (value > highest ? highest : value));
    }
    TIM1->CCR1 = compare[0];
    TIM1->CCR2 = compare[1];
    TIM1->CCR3 = compare[2];
  }

  uint32_t cycles = DWT->CYCCNT - startCycles;
  if (cycles > maxInterruptCycles) maxInterruptCycles = cycles;
}

// Any crash must not leave the PWM running with stale duty cycles.
extern "C" void HardFault_Handler(void) {
  TIM1->BDTR &= ~TIM_BDTR_MOE;
  while (true) {}
}

// ---------------------------------------------------------------- hardware setup
void setupSpi() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_SPI1_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {};
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF5_SPI1;
  gpio.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
  HAL_GPIO_Init(GPIOA, &gpio);

  // Master, 16-bit frames, mode 1 (clock idles low, data sampled on the
  // falling edge), PCLK2 / 64 = 1.4 MHz, chip selects driven by software.
  SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_BR_2 | SPI_CR1_BR_0 | SPI_CR1_CPHA | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_DFF;
  SPI1->CR1 |= SPI_CR1_SPE;
}

void setupAdc() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_ADC1_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {};
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = GPIO_PIN_0;
  HAL_GPIO_Init(GPIOB, &gpio);

  ADC123_COMMON->CCR = ADC_CCR_ADCPRE_0;  // ADC clock = PCLK2 / 4
  // 28-cycle sample time (0b010) on channels 0, 1, 4 and 8.
  ADC1->SMPR2 = (2u << (3 * ADC_CHANNEL_A)) | (2u << (3 * ADC_CHANNEL_B)) | (2u << (3 * ADC_CHANNEL_C)) |
                (2u << (3 * ADC_CHANNEL_BUS));
  // Injected group of three: with JL = 2 the sequence occupies slots 2, 3 and 4.
  ADC1->JSQR = (2u << ADC_JSQR_JL_Pos) | (ADC_CHANNEL_A << ADC_JSQR_JSQ2_Pos) | (ADC_CHANNEL_B << ADC_JSQR_JSQ3_Pos) |
               (ADC_CHANNEL_C << ADC_JSQR_JSQ4_Pos);
  // Regular group: the bus voltage alone, started by software every PWM period.
  ADC1->SQR1 = 0;
  ADC1->SQR3 = ADC_CHANNEL_BUS;
  ADC1->CR1 = ADC_CR1_SCAN | ADC_CR1_JEOCIE;
  // Injected group triggered by the rising edge of TIM1's trigger output (JEXTSEL = 1).
  ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_JEXTEN_0 | ADC_CR2_JEXTSEL_0;
  delay(2);
  ADC1->CR2 |= ADC_CR2_SWSTART;

  HAL_NVIC_SetPriority(ADC_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(ADC_IRQn);
}

// Starts Timer 1 with every phase at 50 % but the outputs still released.
void setupPwmTimer() {
  __HAL_RCC_TIM1_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {};
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_TIM1;
  gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
  HAL_GPIO_Init(GPIOA, &gpio);

  // Timers on APB2 run at twice PCLK2 whenever APB2 is divided.
  uint32_t timerHz = HAL_RCC_GetPCLK2Freq();
  if ((RCC->CFGR & RCC_CFGR_PPRE2) != RCC_CFGR_PPRE2_DIV1) timerHz *= 2;
  pwmTop = (uint32_t)(timerHz / (2.0f * PWM_HZ));

  TIM1->CR1 = TIM_CR1_CMS_0 | TIM_CR1_ARPE;  // centre-aligned, buffered period
  TIM1->PSC = 0;
  TIM1->ARR = pwmTop;
  // Repetition count 1: buffered duty values are taken over once per period
  // instead of at both the peak and the zero.
  TIM1->RCR = 1;
  // Channels 1..3: PWM mode 1 (high while the counter is below the compare value), buffered.
  TIM1->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE | TIM_CCMR1_OC2M_2 | TIM_CCMR1_OC2M_1 |
                TIM_CCMR1_OC2PE;
  // Channel 4: PWM mode 2 (goes high as the counter passes the compare value
  // on the way up). No pin; it is routed to the timer's trigger output, whose
  // rising edge starts the ADC.
  TIM1->CR2 = TIM_CR2_MMS_2 | TIM_CR2_MMS_1 | TIM_CR2_MMS_0;
  TIM1->CCMR2 = TIM_CCMR2_OC3M_2 | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3PE | TIM_CCMR2_OC4M | TIM_CCMR2_OC4PE;
  TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = pwmTop / 2;
  TIM1->CCR4 = pwmTop - (uint32_t)(SAMPLE_LEAD_SECONDS * timerHz);
  TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC3E | TIM_CCER_CC4E;
  TIM1->BDTR = 0;  // main output disabled
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 |= TIM_CR1_CEN;
}

void setObserverBandwidth(float hertz) {
  observerBandwidthHz = hertz;
  float omega = TWO_PI * hertz;
  noInterrupts();
  observerPositionGain = 2.0f * omega * CONTROL_DT;      // critically damped
  observerSpeedGain = omega * omega * CONTROL_DT;        // counts/s per count
  interrupts();
}

void setCurrentBandwidth(float hertz) {
  currentBandwidthHz = hertz;
  float omega = TWO_PI * hertz;
  noInterrupts();
  currentKp = PHASE_HENRIES * omega;
  currentKiDt = PHASE_OHMS * omega * CONTROL_DT;
  interrupts();
}

// ---------------------------------------------------------------- reporting
const char *tripText(Trip reason) {
  switch (reason) {
    case TRIP_CURRENT: return "phase current over limit";
    case TRIP_SPEED: return "speed over limit";
    case TRIP_BUS_HIGH: return "bus voltage too high (braking energy not absorbed)";
    case TRIP_BUS_LOW: return "bus voltage too low";
    case TRIP_ENCODER: return "encoder readings invalid";
    case TRIP_DRIVER: return "driver fault";
    case TRIP_USER: return "stopped by command";
    default: return "none";
  }
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

// Stops (if not already tripped) and reports. The driver's status registers
// were captured at the moment of the trip, before its gate driver was disabled.
void shutDown() {
  noInterrupts();
  trip(TRIP_USER);
  interrupts();
  Serial.print(F("STOPPED: "));
  Serial.println(tripText(tripReason));
  Serial.print(F("bus "));
  Serial.print(busVolts, 2);
  Serial.print(F(" V  rpm "));
  Serial.print(rpm, 0);
  Serial.print(F("  encoder errors "));
  Serial.println(encoderErrors);
  Serial.print(F("driver at trip: warn=0x"));
  Serial.print(tripRegisters[0], HEX);
  Serial.print(F(" vds=0x"));
  Serial.print(tripRegisters[1], HEX);
  Serial.print(F(" ic=0x"));
  Serial.print(tripRegisters[2], HEX);
  Serial.print(F(" vgs=0x"));
  Serial.println(tripRegisters[3], HEX);
  Serial.println(F("motor coasting, outputs off - press the black reset button to start again"));
  while (true) {}
}

void halt(const char *reason) {
  outputsOff();
  Serial.print(F("STOPPED: "));
  Serial.println(reason);
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);
  while (true) {}
}

void printGains() {
  Serial.print(F("gains: P="));
  Serial.print(positionKp, 2);
  Serial.print(F(" K="));
  Serial.print(speedKp, 3);
  Serial.print(F(" I="));
  Serial.print(speedKi, 2);
  Serial.print(F("  current loop "));
  Serial.print(currentBandwidthHz, 0);
  Serial.print(F(" Hz  observer "));
  Serial.print(observerBandwidthHz, 0);
  Serial.print(F(" Hz  dead-time="));
  Serial.print(deadtimeVolts, 3);
  Serial.println(F(" V"));
}

// ---------------------------------------------------------------- encoder calibration
// Turns a current vector slowly forwards then backwards with the rotor
// following, and compares the encoder with the forced angle the whole way.
void calibrateEncoder() {
  Serial.println(F("calibrating encoder: about 15 s, the shaft turns half a revolution each way"));
  const float step = TWO_PI * CALIBRATION_ELECTRICAL_HZ / PWM_HZ;
  const uint32_t legMillis = (uint32_t)(1000.0f * CALIBRATION_TURNS_EACH_WAY / CALIBRATION_ELECTRICAL_HZ);

  noInterrupts();
  forcedTheta = 0;
  forcedStep = 0;
  targetD = CALIBRATION_AMPS;
  targetQ = 0;
  mode = FORCED;
  interrupts();
  delay(700);  // settle on the starting angle
  if (mode != FORCED) return;

  float startDegrees = shaftDegrees;
  noInterrupts();
  for (uint8_t i = 0; i < 2; i++) calibrationSin[i] = calibrationCos[i] = 0;
  forcedStep = step;
  interrupts();
  delay(legMillis);
  float travelDegrees = shaftDegrees - startDegrees;  // signed with the present `direction`
  forcedStep = -step;
  delay(legMillis);
  forcedStep = 0;
  delay(100);

  noInterrupts();
  bool stillRunning = mode == FORCED;
  if (stillRunning) mode = IDLE;
  targetD = 0;
  float sums[2][2] = {{calibrationSin[0], calibrationCos[0]}, {calibrationSin[1], calibrationCos[1]}};
  interrupts();
  if (!stillRunning) return;

  // Which way did the encoder go while the forced angle went forwards?
  int8_t newDirection = (travelDegrees * direction) > 0 ? 1 : -1;
  uint8_t pick = newDirection > 0 ? 0 : 1;
  float angle = atan2f(sums[pick][0], sums[pick][1]);
  if (angle < 0) angle += TWO_PI;
  float samples = 2.0f * legMillis * OUTER_HZ / 1000.0f;
  float quality = sqrtf(sums[pick][0] * sums[pick][0] + sums[pick][1] * sums[pick][1]) / samples;
  float expectedDegrees = 360.0f * CALIBRATION_TURNS_EACH_WAY / POLE_PAIRS;

  Serial.print(F("shaft moved (deg): "));
  Serial.print(fabsf(travelDegrees), 1);
  Serial.print(F(" (expected "));
  Serial.print(expectedDegrees, 1);
  Serial.println(')');
  Serial.print(F("direction: "));
  Serial.println(newDirection > 0 ? F("+1 (encoder counts up with A->B->C)") : F("-1 (encoder counts down with A->B->C)"));
  Serial.print(F("encoder offset (electrical counts): "));
  Serial.println((int32_t)lroundf(angle / COUNTS_TO_RADIANS));
  Serial.print(F("consistency (1.00 = perfect): "));
  Serial.println(quality, 3);

  if (fabsf(fabsf(travelDegrees) - expectedDegrees) > 0.2f * expectedDegrees || quality < 0.8f) {
    Serial.println(F("calibration REJECTED: the rotor did not follow the field cleanly"));
    return;
  }
  noInterrupts();
  direction = newDirection;
  encoderOffset = (int32_t)lroundf(angle / COUNTS_TO_RADIANS) & (ENCODER_COUNTS - 1);
  encoderCalibrated = true;
  interrupts();
  Serial.println(F("calibration accepted"));
}

// ---------------------------------------------------------------- tuning
// Sets the speed and position gains from the measured acceleration per amp,
// the chosen speed-loop bandwidth and the spacing between loops.
void applyTuning() {
  float omega = TWO_PI * speedBandwidthHz;
  noInterrupts();
  speedKp = omega / accelerationPerAmp;           // amps per rpm
  speedKi = speedKp * omega / loopSpacing;        // amps per rpm per second
  positionKp = omega / loopSpacing / 6.0f;        // (1 / s) -> rpm per degree
  interrupts();
}

void printTuning() {
  Serial.print(F("speed loop "));
  Serial.print(speedBandwidthHz, 1);
  Serial.print(F(" Hz, position loop "));
  Serial.print(speedBandwidthHz / loopSpacing, 1);
  Serial.print(F(" Hz (spacing "));
  Serial.print(loopSpacing, 0);
  Serial.print(F("), from "));
  Serial.print(accelerationPerAmp, 0);
  Serial.println(F(" rpm/s per A"));
  printGains();
}

// Speed after waiting, with the time it was actually read.
static float speedAfter(uint32_t waitMillis, uint32_t &micros_) {
  delay(waitMillis);
  noInterrupts();
  float value = rpm;
  micros_ = micros();
  interrupts();
  return value;
}

// Accelerates the motor at two torque levels and measures the two acceleration rates.
void autoTune() {
  if (mode == TRIPPED) return;
  if (fabsf(rpm) > 5.0f) {
    Serial.println(F("stop the motor first (s), then run 't' again"));
    return;
  }
  Serial.println(F("tuning: the shaft spins up briefly"));
  uint32_t t0, t1, t2, t3;
  noInterrupts();
  targetD = 0;
  targetQ = TUNE_HIGH_AMPS;
  mode = TORQUE;
  interrupts();
  float v0 = speedAfter(TUNE_SETTLE_MILLIS, t0);
  float v1 = speedAfter(TUNE_WINDOW_MILLIS, t1);
  targetQ = TUNE_LOW_AMPS;
  float v2 = speedAfter(TUNE_SETTLE_MILLIS, t2);
  float v3 = speedAfter(TUNE_WINDOW_MILLIS, t3);
  noInterrupts();
  if (mode != TRIPPED) {
    targetQ = 0;
    targetRpm = 0;
    mode = SPEED;  // bring it back to rest
  }
  interrupts();
  if (mode == TRIPPED) return;

  float highRate = (v1 - v0) / ((t1 - t0) * 1e-6f);   // rpm per second
  float lowRate = (v3 - v2) / ((t3 - t2) * 1e-6f);
  float perAmp = (highRate - lowRate) / (TUNE_HIGH_AMPS - TUNE_LOW_AMPS);
  Serial.print(F("acceleration at "));
  Serial.print(TUNE_HIGH_AMPS, 1);
  Serial.print(F(" A / "));
  Serial.print(TUNE_LOW_AMPS, 1);
  Serial.print(F(" A (rpm per second): "));
  Serial.print(highRate, 0);
  Serial.print(F(" / "));
  Serial.println(lowRate, 0);
  Serial.print(F("top speed reached (rpm): "));
  Serial.println(v3, 0);
  if (perAmp < 50.0f || lowRate <= 0) {
    Serial.println(F("tuning REJECTED: the motor did not accelerate as expected"));
    return;
  }
  accelerationPerAmp = perAmp;
  Serial.print(F("acceleration per amp (rpm per second per A): "));
  Serial.println(perAmp, 0);
  Serial.print(F("friction, as q current (A): "));
  Serial.println(TUNE_LOW_AMPS - lowRate / perAmp, 2);
  applyTuning();
  printTuning();
}

// ---------------------------------------------------------------- commands
// Reads one command line from the Serial Monitor without blocking.
void handleSerial() {
  static char line[16];
  static uint8_t length;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (length < sizeof(line) - 1) line[length++] = c;
      continue;
    }
    line[length] = 0;
    length = 0;
    char command = line[0];
    float value = atof(line + 1);

    if (command == 'x') shutDown();
    if (command == 'a') calibrateEncoder();
    if (command == 't' && encoderCalibrated) autoTune();
    if (command == 'm') {
      int rows = constrain((int)value, 0, 20);
      statusIntervalMillis = rows > 0 ? 1000 / rows : 0;
      Serial.println(rows > 0 ? F("status rows on") : F("status rows off (m5 turns them back on)"));
    }
    if (command == 'O') {
      setObserverBandwidth(constrain(value, MINIMUM_INNER_RATIO * speedBandwidthHz, 500.0f));
      Serial.print(F("speed observer bandwidth (Hz): "));
      Serial.println(observerBandwidthHz, 0);
    }
    if (command == 'W' || command == 'R') {
      float highest = min(currentBandwidthHz, observerBandwidthHz) / MINIMUM_INNER_RATIO;
      if (command == 'W') speedBandwidthHz = constrain(value, 2.0f, highest);
      if (command == 'R') loopSpacing = constrain(value, 2.0f, 20.0f);
      applyTuning();
      printTuning();
    }
    if (command == 'c') feedForwardEnabled = line[1] != '0';
    if (command == 'P' || command == 'K' || command == 'I' || command == 'B' || command == 'D') {
      if (command == 'P') positionKp = max(value, 0.0f);
      if (command == 'K') speedKp = max(value, 0.0f);
      if (command == 'I') speedKi = max(value, 0.0f);
      if (command == 'B') setCurrentBandwidth(constrain(value, MINIMUM_INNER_RATIO * speedBandwidthHz, 2000.0f));
      if (command == 'D') deadtimeVolts = constrain(value, 0.0f, 0.2f);
      printGains();
    }

    bool motion = command == 'd' || command == 'q' || command == 'v' || command == 'p' || command == 's';
    if (!motion) continue;
    if (!encoderCalibrated) {
      Serial.println(F("run 'a' first to calibrate the encoder"));
      continue;
    }
    noInterrupts();
    if (mode != TRIPPED) {
      if (command == 'd') {
        targetD = clampf(value, TARGET_LIMIT_AMPS);
        if (mode == IDLE) {
          targetQ = 0;
          mode = TORQUE;
        }
      }
      if (command == 'q') {
        targetQ = clampf(value, TARGET_LIMIT_AMPS);
        mode = TORQUE;
      }
      if (command == 'v') {
        targetD = 0;
        targetQ = 0;
        targetRpm = clampf(value, TARGET_LIMIT_RPM);
        mode = SPEED;
      }
      if (command == 'p' || command == 's') {
        targetD = 0;
        targetQ = 0;
        targetDegrees = command == 'p' ? value : shaftDegrees;
        mode = POSITION;
      }
    }
    interrupts();
  }
}

// ---------------------------------------------------------------- setup and main loop
void setup() {
  Serial.begin(115200);

  digitalWrite(PIN_EN_GATE, LOW);
  pinMode(PIN_EN_GATE, OUTPUT);
  digitalWrite(PIN_ENCODER_CS, HIGH);
  pinMode(PIN_ENCODER_CS, OUTPUT);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  pinMode(PIN_DRIVER_CS, OUTPUT);
  pinMode(PIN_NFAULT, INPUT);
  for (uint16_t i = 0; i < 256; i++) sineTable[i] = sinf(i * (TWO_PI / 256));
  setCurrentBandwidth(DEFAULT_CURRENT_BANDWIDTH_HZ);
  setObserverBandwidth(DEFAULT_OBSERVER_BANDWIDTH_HZ);
  applyTuning();
  // Cycle counter, for timing the control interrupt.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  setupSpi();
  delay(100);

  Serial.println(F("\n--- Nucleo FOC ---"));
  writeDriver(REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);
  writeDriver(REG_SHUNT_AMP, SHUNT_AMP_GAIN_10);
  writeDriver(REG_HS_GATE_DRIVE, GATE_DRIVE_STRENGTH);
  writeDriver(REG_LS_GATE_DRIVE, GATE_DRIVE_STRENGTH);
  if (readDriver(REG_GATE_DRIVE) != GATE_DRIVE_PWM_3X || readDriver(REG_SHUNT_AMP) != SHUNT_AMP_GAIN_10 ||
      readDriver(REG_HS_GATE_DRIVE) != GATE_DRIVE_STRENGTH || readDriver(REG_LS_GATE_DRIVE) != GATE_DRIVE_STRENGTH) {
    halt("driver not responding - check motor supply");
  }
  encoderTransfer();
  uint16_t word = encoderTransfer();
  if (__builtin_parity(word) || (word & ENCODER_ERROR_FLAG)) Serial.println(F("warning: encoder reported an error on the first read"));
  Serial.print(F("encoder: "));
  Serial.println(word & ENCODER_DATA_MASK);

  Serial.println(F("Motor connected and clamped, no propeller? Type g to start."));
  while (Serial.read() != 'g') {}

  // PWM inputs are not driven yet, so this turns on the three low-side MOSFETs (motor braked).
  digitalWrite(PIN_EN_GATE, HIGH);
  delay(100);
  if (!digitalRead(PIN_NFAULT) && (readDriver(REG_WARNINGS) & WARNINGS_FAULT_BIT)) halt("driver fault on enable");

  setupAdc();
  setupPwmTimer();

  // Zero offsets, sampled through the control interrupt with no switching.
  uint32_t offsetStart = millis();
  while (offsetCount < OFFSET_SAMPLES) {
    if (millis() - offsetStart > 1000) halt("no current samples - the timer is not triggering the ADC");
  }
  noInterrupts();
  for (uint8_t phase = 0; phase < 3; phase++) offsets[phase] = (float)offsetSum[phase] / offsetCount;
  mode = IDLE;
  interrupts();
  Serial.print(F("zero offsets  A: "));
  Serial.print(offsets[0], 1);
  Serial.print(F("  B: "));
  Serial.print(offsets[1], 1);
  Serial.print(F("  C: "));
  Serial.println(offsets[2], 1);

  TIM1->BDTR |= TIM_BDTR_MOE;  // outputs on, every phase at 50 %
  delay(200);
  Serial.print(F("bus volts: "));
  Serial.println(busVolts, 2);
  printTuning();
  Serial.print(F("stored encoder calibration: offset "));
  Serial.print(encoderOffset);
  Serial.print(F(", direction "));
  Serial.println(direction);
  Serial.println(F("Running. Commands: d q v p s c P K I B D t W R O m x, and a to recalibrate the encoder."));
  Serial.println(F("mode\tdeg*\tdeg\trpm*\trpm\tiq*\tiq\tid\tvq\tbus\tisr us\tdump ms"));
}

void loop() {
  static uint32_t lastPrintMillis;
  static uint8_t faultPolls;

  handleSerial();

  // The fault pin must read low twice running before it counts.
  faultPolls = digitalRead(PIN_NFAULT) ? 0 : faultPolls + 1;
  if (faultPolls >= 2 && mode != TRIPPED) {
    noInterrupts();
    trip(TRIP_DRIVER);
    interrupts();
  }
  if (mode == TRIPPED) shutDown();

  if (statusIntervalMillis > 0 && millis() - lastPrintMillis >= statusIntervalMillis) {
    lastPrintMillis = millis();
    static const char *const names[] = {"offs", "idle", "cal", "torq", "speed", "pos", "trip"};
    Serial.print(names[mode]);
    Serial.print('\t');
    Serial.print(targetDegrees, 1);
    Serial.print('\t');
    Serial.print(shaftDegrees, 1);
    Serial.print('\t');
    Serial.print(targetRpm, 0);
    Serial.print('\t');
    Serial.print(rpm, 0);
    Serial.print('\t');
    Serial.print(commandedQFiltered, 2);
    Serial.print('\t');
    Serial.print(measuredQ, 2);
    Serial.print('\t');
    Serial.print(measuredD, 2);
    Serial.print('\t');
    Serial.print(measuredVoltsQ, 3);
    Serial.print('\t');
    Serial.print(busVolts, 2);
    Serial.print('\t');
    Serial.print(maxInterruptCycles * (1e6f / F_CPU), 1);
    Serial.print('\t');
    Serial.println(dumpTicks * 1000 / (uint32_t)OUTER_HZ);
    maxInterruptCycles = 0;
    dumpTicks = 0;
  }
  delay(1);
}

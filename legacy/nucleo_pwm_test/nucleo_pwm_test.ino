// First powered test on the NUCLEO-F446RE: 20 kHz PWM from the motor-control
// timer, phase currents sampled in step with it, and a measurement of the
// motor's resistance and inductance.
//
// Sequence, after 'g':
//   1. zero offsets and noise of the three current channels (no switching)
//   2. all three phases at 50 % duty: currents should stay at zero
//   3. a voltage vector along phase A is raised until phase A carries
//      TARGET_AMPS (the rotor swings to that position and holds)
//   4. the voltage is stepped between two levels and the current is recorded
//      every 50 us, which gives the winding's resistance and inductance
//
// Timer 1 runs centre-aligned: each phase output is high around the counter's
// zero and low around its peak. The low-side MOSFETs (where the shunts are)
// conduct around the peak, so the ADC is triggered there by timer channel 4,
// through the timer's trigger output.
// The ADC interrupt is the control interrupt.
//
// Wiring: as nucleo_check, with the motor's three wires on MOTA/B/C.

#include <SPI.h>

const uint8_t PIN_ENCODER_CS = D10;
const uint8_t PIN_DRIVER_CS = D9;
const uint8_t PIN_EN_GATE = D6;
const uint8_t PIN_NFAULT = D5;
// PWM on PA8, PA9, PA10 (D7, D8, D2) = TIM1 CH1..3; current sense on PA0, PA1,
// PA4 (A0..A2) = ADC1 channels 0, 1, 4; bus voltage on PB0 (A3) = channel 8.
const uint32_t ADC_CHANNEL_A = 0;
const uint32_t ADC_CHANNEL_B = 1;
const uint32_t ADC_CHANNEL_C = 4;
const uint32_t ADC_CHANNEL_BUS = 8;

const float PWM_HZ = 20000.0f;
const float TARGET_AMPS = 5.0f;        // phase A current for the hold
const float LIMIT_AMPS = 10.0f;        // any phase beyond this switches the outputs off at once
const float MAX_VOLTS = 0.30f;         // cap on phase voltage amplitude, whatever the current reads
const float VOLT_STEP = 0.002f;
const float STEP_LOW_FRACTION = 0.4f;  // the step test goes from this fraction of the hold voltage up to all of it

const float ADC_REF_VOLTS = 3.3f;
const float ADC_COUNTS = 4096.0f;      // 12-bit
const float SHUNT_OHMS = 0.007f;       // R4, R5, R6 on the EVM
const float CSA_GAIN = 20.0f;          // set below through the shunt amplifier register
const float AMPS_PER_COUNT = ADC_REF_VOLTS / ADC_COUNTS / (SHUNT_OHMS * CSA_GAIN);
const float BUS_DIVIDER = (62.0f + 4.99f) / 4.99f;  // R14 / R16 on the EVM
const float FILTER_GAIN = 0.01f;       // low-pass on the printed currents, about 5 ms

// ADC clock is PCLK2 / 4 = 22.5 MHz and each conversion takes 28 + 12 cycles
// (1.8 us). Starting the three conversions 2.7 us before the timer's peak puts
// the middle one on the peak.
const float SAMPLE_LEAD_SECONDS = 2.7e-6f;

const uint16_t CALIBRATION_SAMPLES = 4000;
const uint16_t CAPTURE_SAMPLES = 160;
const uint16_t CAPTURE_STEP_AT = 16;   // sample index at which the voltage steps up

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
const uint16_t GATE_DRIVE_PWM_3X = 0x296;   // power-on default (0x216) with PWM_MODE = 3 inputs
const uint16_t SHUNT_AMP_GAIN_20 = 0x015;   // 20 V/V on all three amplifiers
// Gate drive strength: 0.125 A source, 0.25 A sink (the power-on default, 0x344,
// is 50 / 60 mA). The default switches the MOSFETs so slowly that about 0.5 us
// of every PWM edge is lost, which at 20 kHz swallowed the first 0.17 V of command.
const uint16_t GATE_DRIVE_STRENGTH = 0x377;

const uint16_t ENCODER_READ_ANGLE = 0xFFFF;  // read 0x3FFF, with parity
const uint16_t ENCODER_DATA_MASK = 0x3FFF;

// Both chips: SPI mode 1, MSB first.
const SPISettings DRIVER_SPI(100000, MSBFIRST, SPI_MODE1);
const SPISettings ENCODER_SPI(1000000, MSBFIRST, SPI_MODE1);

uint32_t pwmTop;  // timer counts from zero to peak
float busVolts;
float offsets[3];

// Shared with the ADC interrupt.
volatile float commandVolts;       // phase-A-axis voltage vector, phase amplitude
volatile float filteredAmps[3];
volatile bool tripped;
volatile bool calibrating;
volatile uint32_t calibrationCount;
volatile uint32_t calibrationSum[3];
volatile uint64_t calibrationSumSquares[3];
volatile uint16_t captureIndex = CAPTURE_SAMPLES;
volatile float captureStepVolts;
float captureAmps[CAPTURE_SAMPLES];

// Sets the three duty cycles for a voltage vector along phase A.
// The new values take effect within the following PWM period.
static inline void applyVolts(float volts) {
  float half = 0.5f * pwmTop;
  float perVolt = pwmTop / busVolts;
  TIM1->CCR1 = (uint32_t)(half + volts * perVolt);
  TIM1->CCR2 = (uint32_t)(half - 0.5f * volts * perVolt);
  TIM1->CCR3 = (uint32_t)(half - 0.5f * volts * perVolt);
}

// Timer outputs released: the driver's pull-downs then hold every input low,
// which turns on the three low-side MOSFETs.
static inline void outputsOff() {
  TIM1->BDTR &= ~TIM_BDTR_MOE;
}

// Runs once per PWM period, just after the three currents have been sampled
// at the middle of the low-side on-time.
extern "C" void ADC_IRQHandler(void) {
  if (!(ADC1->SR & ADC_SR_JEOC)) return;
  ADC1->SR = ~ADC_SR_JEOC;
  uint32_t counts[3] = {ADC1->JDR1, ADC1->JDR2, ADC1->JDR3};

  if (calibrating) {
    for (uint8_t phase = 0; phase < 3; phase++) {
      calibrationSum[phase] += counts[phase];
      calibrationSumSquares[phase] += (uint64_t)counts[phase] * counts[phase];
    }
    calibrationCount++;
    return;
  }

  // Positive = out of the driver, into the motor.
  float amps[3];
  for (uint8_t phase = 0; phase < 3; phase++) {
    amps[phase] = (counts[phase] - offsets[phase]) * AMPS_PER_COUNT;
    filteredAmps[phase] += FILTER_GAIN * (amps[phase] - filteredAmps[phase]);
    if (fabsf(amps[phase]) > LIMIT_AMPS) {
      outputsOff();
      tripped = true;
    }
  }

  if (captureIndex < CAPTURE_SAMPLES) {
    if (captureIndex == CAPTURE_STEP_AT) commandVolts = captureStepVolts;
    captureAmps[captureIndex++] = amps[0];
  }

  applyVolts(commandVolts);
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

// The encoder answers in the following frame, so send the command twice.
uint16_t readEncoder() {
  transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE);
  return transfer(PIN_ENCODER_CS, ENCODER_SPI, ENCODER_READ_ANGLE) & ENCODER_DATA_MASK;
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
void halt(const char *reason) {
  outputsOff();
  Serial.print(F("STOPPED: "));
  Serial.println(reason);
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);
  while (true) {}
}

void checkHealth(const char *stage) {
  if (tripped) halt("phase current over limit");
  if (!digitalRead(PIN_NFAULT) && (readDriver(REG_WARNINGS) & WARNINGS_FAULT_BIT)) halt(stage);
}

// One software-triggered conversion of the bus-voltage channel, in volts on the bus.
float readBusVolts() {
  ADC1->SQR3 = ADC_CHANNEL_BUS;
  uint32_t sum = 0;
  const uint16_t samples = 64;
  for (uint16_t i = 0; i < samples; i++) {
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) {}
    sum += ADC1->DR;
  }
  return (float)sum / samples * ADC_REF_VOLTS / ADC_COUNTS * BUS_DIVIDER;
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
  ADC1->SQR1 = 0;  // regular group: one conversion
  ADC1->CR1 = ADC_CR1_SCAN | ADC_CR1_JEOCIE;
  // Injected group triggered by the rising edge of TIM1's trigger output (JEXTSEL = 1).
  ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_JEXTEN_0 | ADC_CR2_JEXTSEL_0;
  delay(2);

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
  // rising edge starts the ADC. The direct channel-4 trigger is not used
  // because it only reaches the ADC while the main output is enabled.
  TIM1->CR2 = TIM_CR2_MMS_2 | TIM_CR2_MMS_1 | TIM_CR2_MMS_0;
  TIM1->CCMR2 = TIM_CCMR2_OC3M_2 | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3PE | TIM_CCMR2_OC4M | TIM_CCMR2_OC4PE;
  TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = pwmTop / 2;
  TIM1->CCR4 = pwmTop - (uint32_t)(SAMPLE_LEAD_SECONDS * timerHz);
  TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC3E | TIM_CCER_CC4E;
  TIM1->BDTR = 0;  // main output disabled
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 |= TIM_CR1_CEN;

  Serial.print(F("timer clock (MHz): "));
  Serial.print(timerHz / 1000000);
  Serial.print(F("  counts to peak: "));
  Serial.print(pwmTop);
  Serial.print(F("  volts per count: "));
  Serial.println(busVolts / pwmTop, 4);
}

void printThreeAmps() {
  float a = filteredAmps[0];
  float b = filteredAmps[1];
  float c = filteredAmps[2];
  Serial.print(a, 2);
  Serial.print('\t');
  Serial.print(b, 2);
  Serial.print('\t');
  Serial.print(c, 2);
  Serial.print('\t');
  Serial.println(a + b + c, 2);
}

void setup() {
  Serial.begin(115200);

  digitalWrite(PIN_EN_GATE, LOW);
  pinMode(PIN_EN_GATE, OUTPUT);
  digitalWrite(PIN_ENCODER_CS, HIGH);
  pinMode(PIN_ENCODER_CS, OUTPUT);
  digitalWrite(PIN_DRIVER_CS, HIGH);
  pinMode(PIN_DRIVER_CS, OUTPUT);
  pinMode(PIN_NFAULT, INPUT);
  SPI.begin();
  delay(100);

  Serial.println(F("\n--- Nucleo PWM and current-sense test ---"));
  writeDriver(REG_GATE_DRIVE, GATE_DRIVE_PWM_3X);
  writeDriver(REG_SHUNT_AMP, SHUNT_AMP_GAIN_20);
  writeDriver(REG_HS_GATE_DRIVE, GATE_DRIVE_STRENGTH);
  writeDriver(REG_LS_GATE_DRIVE, GATE_DRIVE_STRENGTH);
  if (readDriver(REG_GATE_DRIVE) != GATE_DRIVE_PWM_3X || readDriver(REG_SHUNT_AMP) != SHUNT_AMP_GAIN_20 ||
      readDriver(REG_HS_GATE_DRIVE) != GATE_DRIVE_STRENGTH || readDriver(REG_LS_GATE_DRIVE) != GATE_DRIVE_STRENGTH) {
    halt("driver not responding - check motor supply");
  }

  Serial.println(F("Motor connected and clamped, no propeller? Type g to start."));
  while (Serial.read() != 'g') {}

  // PWM inputs are not driven yet, so this turns on the three low-side MOSFETs (motor braked).
  digitalWrite(PIN_EN_GATE, HIGH);
  delay(100);
  checkHealth("driver fault on enable");

  calibrating = true;
  setupAdc();
  busVolts = readBusVolts();
  Serial.print(F("bus volts: "));
  Serial.println(busVolts, 2);
  if (busVolts < 6.0f) halt("bus voltage too low");
  setupPwmTimer();

  // 1. Zero offsets and noise, sampled through the control interrupt with no switching.
  uint32_t calibrationStart = millis();
  while (calibrationCount < CALIBRATION_SAMPLES) {
    if (millis() - calibrationStart > 1000) halt("no current samples - the timer is not triggering the ADC");
  }
  float noiseCounts[3];
  noInterrupts();
  for (uint8_t phase = 0; phase < 3; phase++) {
    float mean = (float)calibrationSum[phase] / calibrationCount;
    float variance = (float)calibrationSumSquares[phase] / calibrationCount - mean * mean;
    offsets[phase] = mean;
    noiseCounts[phase] = sqrtf(max(variance, 0.0f));
  }
  calibrating = false;
  interrupts();
  Serial.println(F("channel\toffset\tnoise (counts RMS)\tnoise (amps RMS)"));
  for (uint8_t phase = 0; phase < 3; phase++) {
    Serial.print("ABC"[phase]);
    Serial.print('\t');
    Serial.print(offsets[phase], 1);
    Serial.print('\t');
    Serial.print(noiseCounts[phase], 2);
    Serial.print('\t');
    Serial.println(noiseCounts[phase] * AMPS_PER_COUNT, 3);
  }

  // 2. Outputs on, every phase at 50 %.
  TIM1->BDTR |= TIM_BDTR_MOE;
  delay(300);
  checkHealth("driver fault when PWM started");
  Serial.println(F("all phases at 50 % (expect zeros):  iA\tiB\tiC\tsum"));
  Serial.print(F("\t\t\t\t    "));
  printThreeAmps();

  // 3. Raise the vector along phase A until it carries the target current.
  Serial.println(F("volts\tiA\tiB\tiC\tsum"));
  float volts = 0;
  uint8_t steps = 0;
  while (volts < MAX_VOLTS && fabsf(filteredAmps[0]) < TARGET_AMPS) {
    volts += VOLT_STEP;
    commandVolts = volts;
    delay(30);
    checkHealth("driver fault during ramp");
    if (++steps % 10 == 0) {
      Serial.print(volts, 3);
      Serial.print('\t');
      printThreeAmps();
    }
  }
  delay(500);
  Serial.print(F("holding "));
  Serial.print(volts, 3);
  Serial.print(F(" V:\t"));
  printThreeAmps();
  Serial.print(F("encoder at electrical zero: "));
  Serial.println(readEncoder());

  // 4. Step the voltage up and record phase A current every PWM period.
  float lowVolts = STEP_LOW_FRACTION * volts;
  commandVolts = lowVolts;
  delay(300);
  captureStepVolts = volts;
  captureIndex = 0;
  while (captureIndex < CAPTURE_SAMPLES) {}
  delay(100);
  checkHealth("driver fault during step test");

  float before = 0;
  for (uint16_t i = 0; i < CAPTURE_STEP_AT; i++) before += captureAmps[i] / CAPTURE_STEP_AT;
  const uint16_t tail = 32;
  float after = 0;
  for (uint16_t i = CAPTURE_SAMPLES - tail; i < CAPTURE_SAMPLES; i++) after += captureAmps[i] / tail;
  float ohms = (volts - lowVolts) / (after - before);
  // Time to cover 63 % of the step, interpolated between samples. The new
  // voltage reaches the motor one period after it is commanded.
  float threshold = before + 0.632f * (after - before);
  float tauPeriods = 0;
  for (uint16_t i = CAPTURE_STEP_AT + 1; i < CAPTURE_SAMPLES; i++) {
    if (captureAmps[i] >= threshold) {
      float fraction = (threshold - captureAmps[i - 1]) / (captureAmps[i] - captureAmps[i - 1]);
      tauPeriods = (i - 1 + fraction) - (CAPTURE_STEP_AT + 1);
      break;
    }
  }
  float tau = tauPeriods / PWM_HZ;

  Serial.print(F("step "));
  Serial.print(lowVolts, 3);
  Serial.print(F(" -> "));
  Serial.print(volts, 3);
  Serial.print(F(" V gave "));
  Serial.print(before, 2);
  Serial.print(F(" -> "));
  Serial.print(after, 2);
  Serial.println(F(" A"));
  Serial.print(F("phase resistance (ohms): "));
  Serial.println(ohms, 4);
  Serial.print(F("time constant (us): "));
  Serial.println(tau * 1e6f, 0);
  Serial.print(F("phase inductance (uH): "));
  Serial.println(ohms * tau * 1e6f, 1);

  Serial.println(F("phase A amps, one sample per 50 us, 10 per row:"));
  for (uint16_t i = 0; i < CAPTURE_SAMPLES; i++) {
    Serial.print(captureAmps[i], 2);
    Serial.print((i + 1) % 10 == 0 ? '\n' : ' ');
  }

  commandVolts = 0;
  delay(200);
  outputsOff();
  printFaults();
  digitalWrite(PIN_EN_GATE, LOW);
  Serial.println(F("outputs off"));
}

void loop() {}

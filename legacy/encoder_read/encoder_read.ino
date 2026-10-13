// AS5048A magnetic encoder -> Arduino over SPI.
//
// Wiring (Uno, hardware SPI on the ICSP header):
//   AS5048A CSn  -> D2
//   AS5048A SCK  -> ICSP SCK  (same net as D13)
//   AS5048A MISO -> ICSP MISO (same net as D12)
//   AS5048A MOSI -> ICSP MOSI (same net as D11)
//   AS5048A VDD, GND -> Uno supply, GND

#include <SPI.h>

const uint8_t PIN_CS = 2;
const uint8_t POLE_PAIRS = 14;

const uint16_t REG_CLEAR_ERROR = 0x0001;
const uint16_t REG_ANGLE = 0x3FFF;

const uint16_t READ_FLAG = 0x4000;
const uint16_t ERROR_FLAG = 0x4000;
const uint16_t DATA_MASK = 0x3FFF;
const uint16_t COUNTS_PER_REV = 16384;

// AS5048A: SPI mode 1, MSB first, up to 10 MHz.
const SPISettings ENCODER_SPI(1000000, MSBFIRST, SPI_MODE1);

// True if the 16-bit word has an odd number of set bits.
bool oddParity(uint16_t word) {
  word ^= word >> 8;
  word ^= word >> 4;
  word ^= word >> 2;
  word ^= word >> 1;
  return word & 1;
}

// Bit 15 is an even parity bit over the other 15.
uint16_t readCommand(uint16_t reg) {
  uint16_t command = READ_FLAG | reg;
  if (oddParity(command)) command |= 0x8000;
  return command;
}

uint16_t transfer(uint16_t out) {
  SPI.beginTransaction(ENCODER_SPI);
  digitalWrite(PIN_CS, LOW);
  uint16_t in = SPI.transfer16(out);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
  delayMicroseconds(1);  // CSn must stay high >= 350 ns between frames
  return in;
}

// The sensor answers each command in the *next* frame, so by always sending
// "read angle" every call returns the angle latched by the previous call.
// Returns false on a parity error or if the sensor raised its error flag.
bool readRawAngle(uint16_t &raw) {
  uint16_t response = transfer(readCommand(REG_ANGLE));

  if (oddParity(response)) return false;

  if (response & ERROR_FLAG) {
    transfer(readCommand(REG_CLEAR_ERROR));  // reading this register clears the flag
    transfer(readCommand(REG_ANGLE));        // re-prime the pipeline
    return false;
  }

  raw = response & DATA_MASK;
  return true;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  SPI.begin();

  transfer(readCommand(REG_ANGLE));  // prime: first response is not an angle
}

void loop() {
  uint16_t raw;

  if (readRawAngle(raw)) {
    float mechanicalDeg = raw * (360.0f / COUNTS_PER_REV);
    // Not yet aligned to the rotor's d-axis; that offset comes in a later step.
    float electricalDeg = (uint16_t)((uint32_t)raw * POLE_PAIRS % COUNTS_PER_REV) * (360.0f / COUNTS_PER_REV);

    Serial.print(raw);
    Serial.print('\t');
    Serial.print(mechanicalDeg, 2);
    Serial.print('\t');
    Serial.println(electricalDeg, 2);
  } else {
    Serial.println(F("read error"));
  }

  delay(50);
}

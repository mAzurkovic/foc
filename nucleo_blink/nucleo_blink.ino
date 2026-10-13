// NUCLEO-F446RE toolchain test: blinks the green user LED (LD2), prints a
// counter over the USB serial port, and reports the blue user button.
// Nothing needs to be wired to the board.

const uint32_t BLINK_MS = 500;

uint32_t count;

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(USER_BTN, INPUT);
  Serial.println(F("\n--- Nucleo F446RE blink test ---"));
  Serial.print(F("CPU clock (MHz): "));
  Serial.println(F_CPU / 1000000);
}

void loop() {
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  Serial.print(F("tick "));
  Serial.print(count++);
  // The button pulls its pin low when pressed.
  Serial.println(digitalRead(USER_BTN) ? F("") : F("  button pressed"));
  delay(BLINK_MS);
}

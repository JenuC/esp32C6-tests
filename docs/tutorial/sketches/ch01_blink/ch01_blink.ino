// Chapter 1 - Blink
//
// Goal: prove the toolchain works end to end before touching the radio.
// If this does not run, nothing later in the tutorial will.
//
// Board: Seeed XIAO ESP32-C6
// Tools > Board: "XIAO_ESP32C6"
//
// The XIAO ESP32-C6 user LED is on GPIO15 and is ACTIVE LOW: writing LOW
// lights it. Change these three lines if your board wires it differently.
#define LED_PIN 15
#define LED_ON  LOW
#define LED_OFF HIGH

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, LED_ON);
  delay(200);
  digitalWrite(LED_PIN, LED_OFF);
  delay(800);
}

// Troubleshooting
//
// Nothing happens at all
//   The C6 needs Arduino-ESP32 core 3.x. Boards Manager > esp32 by Espressif,
//   version 3.0.0 or later. Core 2.x does not know this chip exists.
//
// Upload fails with "No serial data received" or the port vanishes
//   Put the board in bootloader mode by hand: hold BOOT, tap RESET, release
//   BOOT. The port will reappear under a different number. Upload again.
//
// The LED is on when you expect it off
//   Your board is active-high. Swap LED_ON and LED_OFF above.

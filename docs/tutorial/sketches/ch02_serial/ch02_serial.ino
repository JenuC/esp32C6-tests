// Chapter 2 - Serial you can trust
//
// Goal: a serial link that always comes up, so that when the radio goes quiet
// later you know the silence is real and not a dead console.
//
// Everything after this chapter is debugged through Serial. A sniffer whose
// console is broken looks exactly like a sniffer that is receiving nothing,
// and you can waste an afternoon on that confusion.

#define LED_PIN 15
#define LED_ON  LOW
#define LED_OFF HIGH

static uint32_t tick = 0;

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  Serial.begin(115200);

  // Do NOT write `while (!Serial);` on a native-USB board.
  //
  // With "USB CDC On Boot" enabled, `Serial` is the USB device and `!Serial`
  // stays true until a host program opens the port. A board running on a
  // battery or a plain charger will sit in that loop forever and look bricked.
  //
  // Wait, but give up after 2 seconds and run anyway.
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) {
    delay(10);
  }

  // Even once the port is open, the host's terminal usually misses the first
  // few milliseconds of output. Pause before saying anything important.
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 2: serial check ===");
  Serial.printf("chip: %s  cores: %d  flash: %lu bytes\n",
                ESP.getChipModel(), ESP.getChipCores(), ESP.getFlashChipSize());
  Serial.printf("heap: %lu bytes free\n", ESP.getFreeHeap());
  Serial.println("A heartbeat line every second means the link is healthy.");
  Serial.println();
}

void loop() {
  // A visible heartbeat is the single most useful debugging tool in this
  // tutorial. When frames stop arriving in chapter 3, the heartbeat tells you
  // whether the sketch died or the radio did.
  Serial.printf("[%6lu ms] tick %lu   heap %lu\n", millis(), ++tick, ESP.getFreeHeap());

  digitalWrite(LED_PIN, LED_ON);
  delay(50);
  digitalWrite(LED_PIN, LED_OFF);
  delay(950);
}

// Troubleshooting
//
// Serial Monitor is empty but the LED blinks
//   Tools > USB CDC On Boot: "Enabled", then re-upload. With it disabled,
//   `Serial` goes to the UART pins instead of the USB connector.
//
// Output is garbage characters
//   Baud mismatch. The Serial Monitor must be set to 115200.
//
// The first few lines are always missing
//   Normal for USB CDC. That is what the delay(300) above is for. Increase it
//   if your terminal is slow to attach.

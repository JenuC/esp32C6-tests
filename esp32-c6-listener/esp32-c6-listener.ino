#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

uint8_t current_channel = 11; // 802.15.4 starts at Channel 11 (2405 MHz)

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("--- Starting 802.15.4 Spectrum Channel Scanner ---");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  
  // Set initial channel
  esp_ieee802154_set_channel(current_channel);
  esp_ieee802154_receive();
}

void loop() {
  Serial.print("Scanning Channel Dec: ");
  Serial.print(current_channel);
  Serial.print(" (Hex: 0x");
  if (current_channel < 16) Serial.print("0");
  Serial.print(current_channel, HEX);
  Serial.println(")...");

  // Keep receiving on current channel for 2 seconds
  unsigned long start = millis();
  while (millis() - start < 2000) {
    esp_ieee802154_receive();
    delay(50);
  }

  // Hop to next 802.15.4 channel (11 to 26)
  current_channel++;
  if (current_channel > 26) current_channel = 11;

  esp_ieee802154_set_channel(current_channel);
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;

  uint8_t length = frame[0];

  Serial.print("\n*** DETECTED TRAFFIC on Channel ");
  Serial.print(current_channel);
  Serial.print(" *** Length: ");
  Serial.print(length);
  Serial.print(" bytes | Hex: ");

  for (int i = 1; i <= length; i++) {
    if (frame[i] < 0x10) Serial.print("0");
    Serial.print(frame[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  esp_ieee802154_receive(); // Re-arm radio
}

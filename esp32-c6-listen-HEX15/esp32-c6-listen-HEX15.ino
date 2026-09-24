#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("Initializing IEEE 802.15.4 Receiver...");

  // 1. Enable 802.15.4 Radio
  esp_ieee802154_enable();

  // 2. Set Channel to 15 (0x0F)
  esp_ieee802154_set_channel(16);

  // 3. Set PAN ID to 0x0B01
  esp_ieee802154_set_panid(0x0B01);

  // 4. Enable Promiscuous Mode (bypasses frame filters)
  esp_ieee802154_set_promiscuous(true);

  // 5. Start Listening
  esp_ieee802154_receive();

  Serial.println("Listening on Channel 15 (0x0F)...");
}

void loop() {
  // Put radio in receive mode continuously
  esp_ieee802154_receive();
  delay(100);
}

// Callback matching exact prototype: esp_ieee802154_frame_info_t
extern "C" void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;

  uint8_t length = frame[0]; // First byte is payload length

  Serial.print("[RX Signal] Length: ");
  Serial.print(length);
  Serial.print(" bytes | Raw Bytes (Hex): ");

  // Print raw hex bytes
  for (int i = 1; i <= length; i++) {
    if (frame[i] < 0x10) Serial.print("0");
    Serial.print(frame[i], HEX);
    Serial.print(" ");
  }

  // Print ASCII representation
  Serial.print(" | ASCII: ");
  for (int i = 1; i <= length; i++) {
    char c = (char)frame[i];
    if (c >= 32 && c <= 126) {
      Serial.print(c);
    } else {
      Serial.print('.');
    }
  }
  Serial.println();
}

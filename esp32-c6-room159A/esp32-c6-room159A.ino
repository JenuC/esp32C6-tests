#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Enable the IEEE 802.15.4 radio
  esp_ieee802154_enable();

  // Set operational channel to 15 (0x0F)
  esp_ieee802154_set_channel(21);

  // Set PAN ID to 0x0B01
  esp_ieee802154_set_panid(0x0B01);

  Serial.println("IEEE 802.15.4 initialized: Channel 15 (0x0F), PAN ID 0x0B01");
}

void loop() {
  // Construct raw 802.15.4 frame:
  // Payload is string "11" -> ASCII bytes '1' (0x31) and '1' (0x31)
  uint8_t frame[] = {
      0x01, 0x80,       // Frame Control: Data frame, short addressing
      0x01,             // Sequence Number
      0x01, 0x0B,       // Destination PAN ID: 0x0B01 (Little-Endian: low byte first)
      0xFF, 0xFF,       // Destination Address: 0xFFFF (Broadcast)
      '1',  '1'         // Payload: Text string "11"
  };

  // Transmit frame
  if (esp_ieee802154_transmit(frame, false) == ESP_OK) {
    Serial.println("Transmitted payload: \"11\"");
  } else {
    Serial.println("Transmission failed");
  }

  delay(1000);
}

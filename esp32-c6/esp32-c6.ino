#include <Arduino.h>

// Must wrap IDF low-level headers in extern "C" for Arduino C++ compatibility
extern "C" {
  #include "esp_ieee802154.h"
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Enable the IEEE 802.15.4 subsystem
  esp_ieee802154_enable();

  // Set operational channel to 15 (0x0F in hex)
  esp_ieee802154_set_channel(15);

  Serial.println("IEEE 802.15.4 initialized on Channel 0F (15)!");
}

void loop() {
  // Construct raw frame
  uint8_t frame[] = {
      0x0B, 0x01,       // Frame Control
      0x01,             // Sequence Number
      0xFF, 0xFF,       // Destination PAN ID (Broadcast)
      0xFF, 0xFF,       // Destination Address (Broadcast)
      11                // Payload (Byte value 11 / 0x0B)
  };

  // Transmit frame over RF channel 0F
  if (esp_ieee802154_transmit(frame, false) == ESP_OK) {
    Serial.println("Transmitted payload: 11");
  } else {
    Serial.println("Transmission failed");
  }

  delay(1000);
}

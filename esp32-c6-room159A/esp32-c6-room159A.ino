#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

static uint8_t seq_num = 0x01;

// RX callback to monitor any ACKs or responses back from the indicator box
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info) {
  Serial.print("Rx [");
  Serial.print(frame[0]); // First byte is PSDU length
  Serial.print(" bytes]: ");
  for (int i = 1; i <= frame[0]; i++) {
    Serial.printf("%02X ", frame[i]);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize IEEE 802.15.4 Subsystem
  esp_ieee802154_enable();

  // Configure Channel and Addressing Parameters
  esp_ieee802154_set_channel(21);                // Channel 21 (0x15)
  esp_ieee802154_set_panid(0x000B);              // PAN ID 0x000B
  esp_ieee802154_set_short_address(0x0002);     // Local device address
  esp_ieee802154_set_promiscuous(false);         // Enable hardware address filtering
  esp_ieee802154_set_rx_when_idle(true);

  // Enter receive state
  esp_ieee802154_receive();

  Serial.println("=== 802.15.4 Transmit Test Started ===");
  Serial.println("Configured: Channel 21 | PAN ID: 0x000B | Src: 0x0002 | Dst: 0x004E");
}

void loop() {
  // Construct explicit IEEE 802.15.4 Frame
  // Length = 14 bytes (2 FCF + 1 Seq + 2 PAN + 2 Dst + 2 Src + 3 Payload + 2 FCS)
  // uint8_t tx_frame[] = {
  //   14,                 // [0] PSDU Length (Hardware PHY byte)
  //   0x61, 0x88,         // [1-2] Frame Control: Data, Ack Request, Short Addressing
  //   seq_num,            // [3] Sequence Number
  //   0x01, 0x0B,         // [4-5] Destination PAN ID (0x000B)
  //   0x00, 0x4E,         // [6-7] Destination Short Address (0x004E - Indicator Box)
  //   0x02, 0x00,         // [8-9] Source Short Address (0x0002 - ESP32 Controller)
  //   0x00, 0x14, 0xC1,   // [10-12] Payload Command Bytes (Laser ON indicator flag)
  //   0x00, 0x00          // [13-14] FCS / Hardware CRC Placeholder
  // };

uint8_t tx_frame[] = {
    0x61, 0x88,       // Frame Control
    seq_num,           // Sequence

    0x01, 0x0B,       // PAN ID 0x0B01

    0x00, 0x01,       // Destination = 0x0001
    0x00, 0x5D,       // Source = 0x005D

    0x00, 0x0A, 0xBC // Payload = 10
};

  // Attempt transmission
  esp_err_t result = esp_ieee802154_transmit(tx_frame, false);

  if (result == ESP_OK) {
    Serial.printf("TX Success! Seq: 0x%02X | Length: %d bytes\n", seq_num, tx_frame[0]);
  } else {
    Serial.printf("TX Failed | Error code: 0x%X\n", result);
  }

  seq_num++;

  // Return to receive mode to catch incoming ACKs
  esp_ieee802154_receive();

  delay(500); // Send heartbeat signal every 500ms
}
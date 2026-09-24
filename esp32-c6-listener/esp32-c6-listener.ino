#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

uint8_t current_channel = 11; // 802.15.4 starts at Channel 11 (2405 MHz)

// Frames are handed over from the radio callback (ISR context) to loop()
// through this ring buffer. Nothing slow may run inside the callback.
#define QUEUE_LEN     16
#define MAX_FRAME_LEN 128

struct RxFrame {
  uint8_t channel;
  uint8_t len;
  uint8_t data[MAX_FRAME_LEN];
};

static RxFrame queue[QUEUE_LEN];
static volatile uint8_t q_head = 0; // written by ISR
static volatile uint8_t q_tail = 0; // written by loop()

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("--- Starting 802.15.4 Spectrum Channel Scanner ---");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true); // stay in RX instead of dropping to idle

  esp_ieee802154_set_channel(current_channel);
  esp_ieee802154_receive();
}

void drainQueue() {
  while (q_tail != q_head) {
    RxFrame &f = queue[q_tail];

    Serial.print("\n*** DETECTED TRAFFIC on Channel ");
    Serial.print(f.channel);
    Serial.print(" *** Length: ");
    Serial.print(f.len);
    Serial.print(" bytes | Hex: ");

    for (uint8_t i = 0; i < f.len; i++) {
      if (f.data[i] < 0x10) Serial.print("0");
      Serial.print(f.data[i], HEX);
      Serial.print(" ");
    }
    Serial.println();

    q_tail = (q_tail + 1) % QUEUE_LEN;
  }
}

void loop() {
  Serial.print("Scanning Channel Dec: ");
  Serial.print(current_channel);
  Serial.print(" (Hex: 0x");
  if (current_channel < 16) Serial.print("0");
  Serial.print(current_channel, HEX);
  Serial.println(")...");

  // Listen on the current channel for 2 seconds, printing whatever arrives
  unsigned long start = millis();
  while (millis() - start < 2000) {
    drainQueue();
    delay(5);
  }

  // Hop to next 802.15.4 channel (11 to 26)
  current_channel++;
  if (current_channel > 26) current_channel = 11;

  esp_ieee802154_set_channel(current_channel);
  esp_ieee802154_receive(); // re-arm once per channel, not every 50 ms
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;

  uint8_t len = frame[0];
  if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;

  uint8_t next = (q_head + 1) % QUEUE_LEN;
  if (next != q_tail) { // drop rather than overwrite if loop() is behind
    queue[q_head].channel = current_channel;
    queue[q_head].len     = len;
    memcpy(queue[q_head].data, &frame[1], len);
    q_head = next;
  }

  // CRITICAL: hand the buffer back to the driver's RX pool. Without this the
  // pool (CONFIG_IEEE802154_RX_BUFFER_SIZE, default 20) runs dry and the radio
  // stops delivering frames permanently.
  esp_ieee802154_receive_handle_done(frame);
}

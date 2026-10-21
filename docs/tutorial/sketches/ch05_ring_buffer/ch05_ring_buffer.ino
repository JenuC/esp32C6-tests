// Chapter 5 - Getting out of the callback
//
// Goal: stop doing slow work inside the radio callback.
//
// esp_ieee802154_receive_done() runs in driver/interrupt context. It is not a
// task. While you are inside it the radio is not being serviced, so every
// microsecond spent there is a chance to miss the next frame.
//
// Printing is catastrophically slow by that standard. A 14-byte hex dump is
// about 42 characters; at 115200 baud that is roughly 3.6 ms of blocking,
// which is longer than the air time of the frame that triggered the callback.
// On a busy channel chapters 3 and 4 drop frames continuously.
//
// The fix is the standard embedded answer: the callback copies the bytes
// somewhere safe and returns immediately. loop() does the talking.

#include <Arduino.h>
#include <string.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define CHANNEL       21
#define QUEUE_LEN     16    // frames buffered between loop() passes
#define MAX_FRAME_LEN 128   // 802.15.4 caps the PSDU at 127 bytes

struct RxFrame {
  uint8_t  channel;
  uint8_t  len;
  int8_t   rssi;
  uint8_t  lqi;
  uint32_t at_ms;
  uint8_t  data[MAX_FRAME_LEN];
};

static RxFrame queue[QUEUE_LEN];

// A single-producer, single-consumer ring buffer. This is the reason no lock
// is needed: the callback only ever writes q_head, loop() only ever writes
// q_tail, and neither touches the other's index. Both are volatile so the
// compiler cannot cache them across the while test.
static volatile uint8_t q_head = 0;  // written by the callback only
static volatile uint8_t q_tail = 0;  // written by loop() only

static uint32_t total_rx = 0, dropped = 0;

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 5: queue handoff ===");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_receive();

  Serial.printf("Listening on channel %d, printing from loop()\n", CHANNEL);
  Serial.println();
}

void drainQueue() {
  while (q_tail != q_head) {
    RxFrame &f = queue[q_tail];

    Serial.printf("[%6lu ms] ch %u  len %2u  rssi %4d dBm  lqi %3u  |",
                  f.at_ms, f.channel, f.len, f.rssi, f.lqi);
    for (uint8_t i = 0; i < f.len; i++) {
      Serial.printf(" %02X", f.data[i]);
    }
    Serial.println();

    // Advance only after the frame is fully printed. If this moved first, the
    // callback could overwrite the slot mid-print and tear the output.
    q_tail = (q_tail + 1) % QUEUE_LEN;
  }
}

void loop() {
  drainQueue();

  static uint32_t last_stat = 0;
  if (millis() - last_stat > 5000) {
    last_stat = millis();
    Serial.printf("-- %lu received, %lu dropped (queue full)\n", total_rx, dropped);
  }

  delay(5);
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame,
                                            esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;

  total_rx++;

  // Clamp before the memcpy. frame[0] arrives off the air, and a corrupt
  // length would otherwise run off the end of RxFrame.data. In normal
  // operation this never fires, which is exactly why it has to be here.
  uint8_t len = frame[0];
  if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;

  uint8_t next = (q_head + 1) % QUEUE_LEN;
  if (next == q_tail) {
    // Queue full: drop the new frame rather than overwrite the one loop() may
    // be printing. Losing the newest frame is recoverable; a torn frame is a
    // bug report you will not be able to reproduce.
    dropped++;
  } else {
    queue[q_head].channel = CHANNEL;
    queue[q_head].len     = len;
    queue[q_head].at_ms   = millis();
    // frame_info carries the receiver's own measurements. Chapter 8 is about
    // why you read them from here and not from the end of the frame.
    queue[q_head].rssi    = frame_info ? frame_info->rssi : 0;
    queue[q_head].lqi     = frame_info ? frame_info->lqi  : 0;
    memcpy(queue[q_head].data, &frame[1], len);
    q_head = next;
  }

  esp_ieee802154_receive_handle_done(frame);
}

// Exercise
//
// Set QUEUE_LEN to 2 and point this at a busy channel. The dropped counter
// starts climbing, and you can watch the trade-off directly: the queue only
// has to be long enough to cover one pass of loop().

// Chapter 6 - Scanning the band
//
// Goal: stop guessing which channel to listen on.
//
// 802.15.4 has 16 channels, 11 to 26, spaced 5 MHz apart from 2405 MHz:
//
//     centre frequency = 2405 + 5 * (channel - 11) MHz
//
// Sitting on one channel finds nothing if you picked the wrong one, and you
// cannot tell that case apart from "there is no traffic anywhere". So hop.
//
// The honest arithmetic: 16 channels at 2 seconds each is a 32-second sweep,
// so any one channel is watched 6.25% of the time. That is fine for devices
// that beacon on a timer - two seconds of dwell catches a 1 Hz heartbeat once
// or twice. It is useless for catching a one-off event, which you will simply
// miss 15 times out of 16. Once the scanner has told you where the traffic is,
// go back to chapter 5 and pin the radio to that channel.

#include <Arduino.h>
#include <string.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define FIRST_CHANNEL 11
#define LAST_CHANNEL  26
#define DWELL_MS      2000

#define QUEUE_LEN     32
#define MAX_FRAME_LEN 128

struct RxFrame {
  uint8_t channel;
  uint8_t len;
  int8_t  rssi;
  uint8_t lqi;
  uint8_t data[MAX_FRAME_LEN];
};

static RxFrame queue[QUEUE_LEN];
static volatile uint8_t q_head = 0, q_tail = 0;

// current_channel is read by the callback and written by loop(). A single
// aligned byte, so the read cannot tear; volatile stops the compiler caching
// it. Note the frame is stamped with the channel at receive time - this is why
// a frame printed just after a hop still reports the channel it arrived on.
static volatile uint8_t current_channel = FIRST_CHANNEL;

// Per-channel tally, indexed by channel number for readability.
static uint32_t seen[LAST_CHANNEL + 1] = {0};

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 6: channel scanner ===");
  Serial.printf("Sweeping %d..%d, %d ms each (%d ms per full sweep)\n",
                FIRST_CHANNEL, LAST_CHANNEL, DWELL_MS,
                (LAST_CHANNEL - FIRST_CHANNEL + 1) * DWELL_MS);
  Serial.println();

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_set_channel(current_channel);
  esp_ieee802154_receive();
}

void drainQueue() {
  while (q_tail != q_head) {
    RxFrame &f = queue[q_tail];
    Serial.printf("  ch %2u  len %2u  rssi %4d dBm  lqi %3u  |",
                  f.channel, f.len, f.rssi, f.lqi);
    for (uint8_t i = 0; i < f.len; i++) Serial.printf(" %02X", f.data[i]);
    Serial.println();
    q_tail = (q_tail + 1) % QUEUE_LEN;
  }
}

void printSummary() {
  Serial.println();
  Serial.println("---- frames per channel so far ----");
  for (uint8_t c = FIRST_CHANNEL; c <= LAST_CHANNEL; c++) {
    if (seen[c] == 0) continue;
    Serial.printf("  ch %2u (0x%02X, %d MHz): %lu\n",
                  c, c, 2405 + 5 * (c - 11), seen[c]);
  }
  Serial.println("-----------------------------------");
  Serial.println();
}

void loop() {
  Serial.printf("[ch %2u / 0x%02X] listening...\n", current_channel, current_channel);

  uint32_t start = millis();
  while (millis() - start < DWELL_MS) {
    drainQueue();
    delay(5);
  }
  drainQueue();  // once more, so nothing is carried across the hop

  uint8_t next = current_channel + 1;
  if (next > LAST_CHANNEL) {
    next = FIRST_CHANNEL;
    printSummary();   // one summary per completed sweep
  }

  current_channel = next;
  esp_ieee802154_set_channel(next);

  // Arm once, here, after the channel change. Calling receive() on a timer
  // inside the dwell loop is a common habit and it is pointless: with
  // set_rx_when_idle(true) the radio stays in RX by itself.
  esp_ieee802154_receive();
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame,
                                            esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;

  uint8_t len = frame[0];
  if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;

  uint8_t ch = current_channel;
  if (ch <= LAST_CHANNEL) seen[ch]++;

  uint8_t next = (q_head + 1) % QUEUE_LEN;
  if (next != q_tail) {
    queue[q_head].channel = ch;
    queue[q_head].len     = len;
    queue[q_head].rssi    = frame_info ? frame_info->rssi : 0;
    queue[q_head].lqi     = frame_info ? frame_info->lqi  : 0;
    memcpy(queue[q_head].data, &frame[1], len);
    q_head = next;
  }

  esp_ieee802154_receive_handle_done(frame);
}

// Reading the output
//
// Channels carrying real networks light up consistently every sweep. A single
// frame on one channel in one sweep is usually a neighbour's device or noise
// that happened to pass CRC.
//
// Wi-Fi overlaps this band. A channel that is quiet here may simply be buried
// under a Wi-Fi access point rather than unused.

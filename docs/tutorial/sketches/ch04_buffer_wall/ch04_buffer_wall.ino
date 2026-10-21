// Chapter 4 - The twenty frame wall
//
// Goal: fix chapter 3. One line.
//
// Chapter 3 stopped after exactly 20 frames, and it stopped forever - not for
// a second, and not only on that channel.
//
// Why 20: the pointer handed to the callback is one of a pool of statically
// allocated receive buffers. The pool size is CONFIG_IEEE802154_RX_BUFFER_SIZE
// and its default is 20. Each buffer stays checked out to your code until you
// hand it back. Chapter 3 never did, so after 20 frames the driver had nowhere
// left to put the 21st, and the radio went permanently deaf.
//
// The giveaway in a log is a count that stops on a suspiciously round number
// and never moves again. A crash looks different: the heartbeat stops too.
// Here the heartbeat kept running, which is what proves the sketch was alive
// and the radio was not.

#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define CHANNEL 21

static volatile uint32_t frames_seen = 0;

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 4: buffers returned ===");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_receive();

  Serial.printf("Listening on channel %d. The counter should pass 20 and keep going.\n",
                CHANNEL);
  Serial.println();
}

void loop() {
  Serial.printf("[%6lu ms] alive, frames so far: %lu\n", millis(), frames_seen);
  delay(2000);
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame,
                                            esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;

  frames_seen++;
  uint8_t len = frame[0];

  Serial.printf("#%lu  len %u  |", frames_seen, len);
  for (uint8_t i = 1; i <= len; i++) {
    Serial.printf(" %02X", frame[i]);
  }
  Serial.println();

  // THE FIX. Return the buffer to the pool.
  //
  // Needs ESP-IDF 5.1 or later, which means Arduino-ESP32 core 3.x. If this
  // line does not compile, your core is too old; upgrade rather than trying to
  // work around it, because on older cores there is no way to free a buffer.
  //
  // Release it *after* you are done reading `frame`. Everything you still need
  // must be copied out first - which is exactly the problem chapter 5 solves
  // properly.
  esp_ieee802154_receive_handle_done(frame);
}

// Things worth knowing about this fix
//
// It is still not a good callback. Printing from here blocks the radio, and
// chapter 5 deals with that. But it no longer dies, and a sniffer that drops
// some frames is infinitely more useful than one that stops after 20.
//
// Raising CONFIG_IEEE802154_RX_BUFFER_SIZE is not a fix. It buys you a bigger
// number to stall at. A leak is a leak.

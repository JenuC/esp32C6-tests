// Chapter 3 - First frames out of the air
//
// Goal: turn on the 802.15.4 radio and print raw frames.
//
// *** THIS SKETCH CONTAINS A DELIBERATE BUG. ***
//
// It works, then stops. Run it, watch the counter, and write down the number
// it stops at before reading chapter 4. The number is the whole lesson.

#include <Arduino.h>

// IDF headers are C. Without extern "C" the linker will not find these
// symbols from a C++ .ino file.
extern "C" {
  #include "esp_ieee802154.h"
}

// 802.15.4 channels are 11..26. Pick one with traffic on it; if you do not
// know, chapter 6 builds a scanner that finds out for you.
#define CHANNEL 21

static volatile uint32_t frames_seen = 0;

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 3: first frames ===");

  esp_ieee802154_enable();              // power up the 802.15.4 subsystem

  // Promiscuous mode switches off hardware address filtering, so every frame
  // on the channel is delivered instead of only frames addressed to us. This
  // is what makes a sniffer possible.
  //
  // Side effect: it also disables automatic ACKs. A sniffer is invisible, it
  // never replies. And once promiscuous is on, set_panid() and
  // set_short_address() no longer change what you receive.
  esp_ieee802154_set_promiscuous(true);

  esp_ieee802154_set_rx_when_idle(true); // go back to RX on its own after each frame
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_receive();              // enter the receive state

  Serial.printf("Listening on channel %d (%d MHz)\n",
                CHANNEL, 2405 + 5 * (CHANNEL - 11));
  Serial.println("Watch the frame counter. Note where it stops.");
  Serial.println();
}

void loop() {
  // A heartbeat, so you can tell "the radio stopped" from "the sketch died".
  Serial.printf("[%6lu ms] alive, frames so far: %lu\n", millis(), frames_seen);
  delay(2000);
}

// The driver calls this when a frame passes CRC.
//
// frame[0]      = PSDU length, counting the 2-byte FCS field
// frame[1..len] = the PSDU itself
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

  // *** THE BUG IS THAT NOTHING HAPPENS HERE. ***
  //
  // `frame` points into a buffer the driver lent us, and we never give it
  // back. Chapter 4 explains what that costs.
}

// Troubleshooting
//
// "esp_ieee802154.h: No such file or directory"
//   You are building for the wrong chip. The radio exists on the C6, C5 and
//   H2. An S3 or a classic ESP32 has no 802.15.4 hardware at all.
//
// No frames ever appear
//   Either there is genuinely nothing on this channel (likely - try chapter 6)
//   or the transmitter is out of range. Both look identical from here.

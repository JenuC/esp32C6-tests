// Chapter 8 - The two bytes that are not a CRC
//
// Goal: settle an argument with the spec, using data.
//
// Every 802.15.4 frame ends with a 2-byte Frame Check Sequence, and frame[0]
// counts it. So the obvious reading of a 14-byte PSDU is 12 bytes of MAC plus
// a 2-byte CRC, and most people write their parser that way.
//
// That is correct on the air and wrong in the receive buffer. The hardware has
// already verified the CRC before the callback runs - that is what "receive
// done" means - and it reuses those two bytes to report its own measurements:
// RSSI, then a link-quality value.
//
// Do not take this on faith, and do not take it from me. This sketch proves it
// from the air in front of you.
//
// The experiment: remember the leading bytes of every frame, and watch for the
// same frame body arriving twice with different trailing bytes. A CRC is a
// function of frame content, so identical content cannot produce two different
// checksums. One counterexample settles it.
//
// Repeats are common because 802.15.4 retransmits when an ACK is lost, so the
// same bytes really do go out twice. Point this at a busy channel and you
// normally get a hit within a minute.

#include <Arduino.h>
#include <string.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define CHANNEL       21
#define QUEUE_LEN     32
#define MAX_FRAME_LEN 128

#define TRACK_BODIES  24   // distinct frame bodies remembered
#define TRACK_KEY     16   // leading bytes used to identify a body
#define TRACK_TAILS    6   // distinct trailing pairs recorded per body

struct RxFrame {
  uint8_t len;
  int8_t  rssi;
  uint8_t lqi;
  uint8_t data[MAX_FRAME_LEN];
};
static RxFrame queue[QUEUE_LEN];
static volatile uint8_t q_head = 0, q_tail = 0;

struct BodyRecord {
  bool    used;
  uint8_t keylen;
  uint8_t key[TRACK_KEY];
  uint8_t ntails;
  uint8_t tails[TRACK_TAILS][2];
  uint32_t hits;
};
static BodyRecord tracked[TRACK_BODIES];
static uint32_t proofs = 0;

// Running RSSI stats, to show the second use of these bytes.
static int32_t  rssi_sum = 0;
static uint32_t rssi_n = 0;
static int8_t   rssi_min = 127, rssi_max = -128;

static BodyRecord *findOrAdd(const uint8_t *body, uint8_t n) {
  if (n > TRACK_KEY) n = TRACK_KEY;
  for (uint8_t i = 0; i < TRACK_BODIES; i++) {
    if (tracked[i].used && tracked[i].keylen == n &&
        memcmp(tracked[i].key, body, n) == 0) {
      return &tracked[i];
    }
  }
  for (uint8_t i = 0; i < TRACK_BODIES; i++) {
    if (!tracked[i].used) {
      tracked[i].used = true;
      tracked[i].keylen = n;
      memcpy(tracked[i].key, body, n);
      tracked[i].ntails = 0;
      tracked[i].hits = 0;
      return &tracked[i];
    }
  }
  return NULL;   // table full; stop learning rather than thrash
}

static void examine(const RxFrame &f) {
  if (f.len < 5) return;
  const uint8_t body_len = f.len - 2;              // MAC content
  const uint8_t *tail = &f.data[body_len];         // the two bytes in question

  rssi_sum += f.rssi; rssi_n++;
  if (f.rssi < rssi_min) rssi_min = f.rssi;
  if (f.rssi > rssi_max) rssi_max = f.rssi;

  BodyRecord *r = findOrAdd(f.data, body_len);
  if (!r) return;
  r->hits++;

  bool known = false;
  for (uint8_t i = 0; i < r->ntails; i++) {
    if (r->tails[i][0] == tail[0] && r->tails[i][1] == tail[1]) { known = true; break; }
  }

  if (!known && r->ntails < TRACK_TAILS) {
    r->tails[r->ntails][0] = tail[0];
    r->tails[r->ntails][1] = tail[1];
    r->ntails++;

    if (r->ntails == 2) {
      // Two different trailers for one frame body. That is the proof.
      proofs++;
      Serial.println();
      Serial.println("  **** PROOF: identical frame body, different trailing bytes ****");
      Serial.print("  body:");
      for (uint8_t i = 0; i < r->keylen; i++) Serial.printf(" %02X", r->key[i]);
      Serial.println();
      for (uint8_t i = 0; i < r->ntails; i++) {
        int8_t as_rssi = (int8_t)r->tails[i][0];
        Serial.printf("    trailer %02X %02X   -> rssi %d dBm, lqi %u\n",
                      r->tails[i][0], r->tails[i][1], as_rssi, r->tails[i][1]);
      }
      Serial.println("  A CRC is a function of the frame content, so one body");
      Serial.println("  cannot have two checksums. These are receiver metadata.");
      Serial.println();
    } else if (r->ntails > 2) {
      Serial.printf("  (same body now seen with %u different trailers)\n", r->ntails);
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 8: RSSI, LQI, and the trailing bytes ===");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_receive();

  Serial.printf("Listening on channel %d. Waiting for a repeated frame...\n\n", CHANNEL);
}

void loop() {
  while (q_tail != q_head) {
    RxFrame &f = queue[q_tail];

    // Two readings of the same two bytes, side by side. The frame_info values
    // are the supported ones; the tail values are what a naive parser would
    // mistake for a checksum.
    const uint8_t *tail = (f.len >= 5) ? &f.data[f.len - 2] : f.data;
    Serial.printf("len %2u  frame_info: rssi %4d dBm lqi %3u"
                  "   tail bytes: %02X %02X  (%4d dBm, %3u)\n",
                  f.len, f.rssi, f.lqi, tail[0], tail[1], (int8_t)tail[0], tail[1]);

    examine(f);
    q_tail = (q_tail + 1) % QUEUE_LEN;
  }

  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    if (rssi_n) {
      Serial.printf("\n-- %lu frames, rssi mean %ld dBm, min %d, max %d, proofs found %lu\n\n",
                    rssi_n, (long)(rssi_sum / (int32_t)rssi_n), rssi_min, rssi_max, proofs);
    }
  }
  delay(5);
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame,
                                            esp_ieee802154_frame_info_t *frame_info) {
  if (frame == NULL) return;
  uint8_t len = frame[0];
  if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;

  uint8_t next = (q_head + 1) % QUEUE_LEN;
  if (next != q_tail) {
    queue[q_head].len  = len;
    queue[q_head].rssi = frame_info ? frame_info->rssi : 0;
    queue[q_head].lqi  = frame_info ? frame_info->lqi  : 0;
    memcpy(queue[q_head].data, &frame[1], len);
    q_head = next;
  }
  esp_ieee802154_receive_handle_done(frame);
}

// What to do with this
//
// Read frame_info->rssi and frame_info->lqi. They are the documented route to
// the same numbers and they do not depend on a buffer layout detail.
//
// Treat the payload as (frame[0] - 2) bytes long. Getting this wrong by two
// bytes is how a protocol with a 3-byte payload looks like it has a 5-byte one,
// with two fields that never quite make sense.
//
// RSSI is useful beyond debugging: it is roughly a distance proxy. In the
// capture logs in this repo, one room reads about -62 dBm and a room two doors
// away reads about -95 dBm on the same hardware, which was enough to work out
// which transmitter was which.
//
// On transmit the FCS is real. esp_ieee802154_transmit() appends a genuine
// CRC over your frame - the asymmetry is only on the receive side.

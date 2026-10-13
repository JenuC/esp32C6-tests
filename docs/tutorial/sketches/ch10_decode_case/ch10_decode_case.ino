// Chapter 10 - Decoding an unknown payload
//
// Goal: the method. Everything before this chapter reads the parts of a frame
// that a standard defines. This chapter is about the part nobody documented.
//
// The case study is real and the data is in this repository, under
// esp32-c6-listener/esp-listener-output/ - about 12,800 frames from Digi XB24
// radios wired to laser-safety monitors, beaconing status to an indicator box
// roughly once a second. The payload format was not written down anywhere.
//
// After chapter 7 the frame splits like this, and only the last part is a
// mystery:
//
//   61 88 46 01 0B 00 00 01 00 | 83 00 0B | C1 0B
//   MAC header (802.15.4)        payload    RSSI/LQI (chapter 8)
//
// Three bytes. Here is how they fell.
//
// -------------------------------------------------------------------------
// STEP 1 - COUNT BEFORE YOU READ
//
// Group frames by length and by source address, and find the fields that never
// change across thousands of frames. Constants are structure; variables are
// content. In this capture:
//
//   - only two frame lengths existed, 14 and 5, in near-equal numbers.
//     Equal counts of two sizes is the signature of acknowledged unicast, so
//     the 5-byte frames were ACKs and there was really only one format.
//   - the destination was 0x0000 in all 6,426 data frames. A constant.
//   - the middle payload byte was 0x00 in all 6,426. A constant.
//   - exactly three source addresses existed: 0x0001, 0x0002, 0x0003.
//
// That is most of the work, and none of it required a guess.
//
// -------------------------------------------------------------------------
// STEP 2 - CHANGE THE WORLD AND CAPTURE AGAIN
//
// This is the step that actually decodes a protocol, and it beats any amount
// of passive staring. Three captures were taken under known conditions:
//
//   capture                   src 0x0001   src 0x0002   src 0x0003
//   monitors off              0A (10)      14 (20)      1E (30)
//   monitors on, laser off    0A (10)      14 (20)      1E (30)
//   monitors on, LASER ON     0B (11)      14 (20)      1E (30)
//                             ^^^^^^^
//
// One byte moved, on one device, in the one capture where that device's laser
// was on. The protocol is:
//
//     payload[2] = source_address * 10 + (laser_on ? 1 : 0)
//
// So 10/11 for monitor 1, 20/21 for monitor 2, 30/31 for monitor 3. The values
// read as decimal in hex (0x0A = 10, 0x14 = 20), which is a tell that a person
// chose them rather than a bit-packing scheme.
//
// One passive capture of 12,000 frames could not have produced this. One
// capture with a known physical change did it in a single table.
//
// -------------------------------------------------------------------------
// STEP 3 - WHAT IS LEFT OVER IS USUALLY A COUNTER
//
// payload[0] varied every frame. Comparing consecutive frames from the same
// source showed it advancing in step with the MAC sequence number about 95% of
// the time, from a different starting value, with the offset slipping by one
// every few hundred frames.
//
// That is a second counter, maintained one layer above the MAC, not anything
// derived from it. Because the offset drifts it cannot be computed - so a
// re-implementation just keeps its own counter, which is what the real radios
// are doing too.
//
// The remaining 5% were exact duplicate frames: MAC retransmissions after a
// lost ACK. Those same duplicates are what proved the RSSI/LQI point in
// chapter 8.
//
// -------------------------------------------------------------------------
// WHAT THIS SKETCH DOES
//
// Two things at once:
//
//   DECODE  - applies the finished protocol above and prints it in English.
//   LEARN   - tabulates, per source address, every value each payload byte has
//             ever taken. That table is the tool from step 2, and it is how you
//             do this to a device of your own. Run it, change something
//             physical, and watch which column moves.

#include <Arduino.h>
#include <string.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define CHANNEL       21     // where these monitors live; chapter 6 finds yours
#define QUEUE_LEN     32
#define MAX_FRAME_LEN 128

#define MAX_SOURCES    8     // distinct source addresses tracked
#define MAX_PAYLOAD    8     // payload bytes profiled
#define MAX_VALUES     8     // distinct values remembered per byte

struct RxFrame {
  uint8_t len;
  int8_t  rssi;
  uint8_t lqi;
  uint8_t data[MAX_FRAME_LEN];
};
static RxFrame queue[QUEUE_LEN];
static volatile uint8_t q_head = 0, q_tail = 0;

// ------------------------------------------------- minimal header parse
// Cut down from chapter 7 to the one case these frames use: data frame, PAN
// compression on, short addressing both ends. Chapter 7 has the general parser.

struct Mac {
  bool     ok;
  uint8_t  type, seq;
  uint16_t pan, dst, src;
  uint8_t  payload_off, payload_len;
};

static bool parseShortAddressed(const uint8_t *psdu, uint8_t psdu_len, Mac *m) {
  memset(m, 0, sizeof(*m));
  if (psdu_len < 5) return false;
  const uint8_t mac_len = psdu_len - 2;      // last 2 bytes are RSSI/LQI

  uint16_t fcf = (uint16_t)psdu[0] | ((uint16_t)psdu[1] << 8);
  m->type = fcf & 0x07;
  m->seq  = psdu[2];

  const uint8_t dst_mode = (fcf >> 10) & 3;
  const uint8_t src_mode = (fcf >> 14) & 3;
  const bool    compress = (fcf >> 6) & 1;

  if (m->type == 2) { m->ok = true; return true; }           // ACK: nothing else
  if (dst_mode != 2 || src_mode != 2 || !compress) return false;  // not our shape
  if (mac_len < 9) return false;

  m->pan = (uint16_t)psdu[3] | ((uint16_t)psdu[4] << 8);
  m->dst = (uint16_t)psdu[5] | ((uint16_t)psdu[6] << 8);
  m->src = (uint16_t)psdu[7] | ((uint16_t)psdu[8] << 8);
  m->payload_off = 9;
  m->payload_len = mac_len - 9;
  m->ok = true;
  return true;
}

// ------------------------------------------------- the decoded protocol

static bool decodeMonitor(const Mac &m, const uint8_t *psdu,
                          uint8_t *monitor_id, bool *laser_on) {
  if (m.payload_len != 3) return false;
  const uint8_t cmd = psdu[m.payload_off + 2];

  // Invert cmd = id * 10 + on, and only accept it if it round-trips. A value
  // like 0x37 (55) is not this protocol and should be reported as unknown
  // rather than silently decoded as "monitor 5, laser on".
  const uint8_t id = cmd / 10;
  const uint8_t on = cmd % 10;
  if (on > 1 || id == 0 || id > 9) return false;
  if (id * 10 + on != cmd) return false;

  // Cross-check against the MAC source address, which is the XBee's MY
  // parameter and carries the same id. Agreement is a good sign the decode is
  // right; disagreement means the model is wrong, so say so.
  if (m.src != id) return false;

  *monitor_id = id;
  *laser_on   = (on == 1);
  return true;
}

// ------------------------------------------------- the learning table

struct ByteProfile {
  uint8_t nvalues;
  uint8_t values[MAX_VALUES];
  bool    overflowed;     // more distinct values than we can hold => varying
};

struct SourceProfile {
  bool     used;
  uint16_t src;
  uint32_t frames;
  uint8_t  payload_len;
  ByteProfile byte[MAX_PAYLOAD];
};
static SourceProfile sources[MAX_SOURCES];

static void learn(const Mac &m, const uint8_t *psdu) {
  SourceProfile *s = NULL;
  for (uint8_t i = 0; i < MAX_SOURCES; i++) {
    if (sources[i].used && sources[i].src == m.src) { s = &sources[i]; break; }
  }
  if (!s) {
    for (uint8_t i = 0; i < MAX_SOURCES; i++) {
      if (!sources[i].used) {
        sources[i].used = true;
        sources[i].src = m.src;
        s = &sources[i];
        Serial.printf("  (new source 0x%04X)\n", m.src);
        break;
      }
    }
  }
  if (!s) return;

  s->frames++;
  s->payload_len = m.payload_len;

  uint8_t n = m.payload_len;
  if (n > MAX_PAYLOAD) n = MAX_PAYLOAD;
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t v = psdu[m.payload_off + i];
    ByteProfile *b = &s->byte[i];
    bool known = false;
    for (uint8_t k = 0; k < b->nvalues; k++) {
      if (b->values[k] == v) { known = true; break; }
    }
    if (known) continue;
    if (b->nvalues < MAX_VALUES) b->values[b->nvalues++] = v;
    else                         b->overflowed = true;
  }
}

static void printProfile() {
  Serial.println();
  Serial.println("================ payload profile ================");
  Serial.println("Constant columns are structure. Columns with a handful of");
  Serial.println("values are state - change something physical and watch one");
  Serial.println("of them move. Columns marked 'varies' are counters.");
  Serial.println();
  for (uint8_t i = 0; i < MAX_SOURCES; i++) {
    SourceProfile &s = sources[i];
    if (!s.used) continue;
    Serial.printf("  src 0x%04X  %lu frames, %u-byte payload\n",
                  s.src, s.frames, s.payload_len);
    uint8_t n = s.payload_len > MAX_PAYLOAD ? MAX_PAYLOAD : s.payload_len;
    for (uint8_t b = 0; b < n; b++) {
      ByteProfile &p = s.byte[b];
      Serial.printf("    payload[%u]: ", b);
      if (p.overflowed) {
        Serial.printf("varies (>%u distinct values) - likely a counter\n", MAX_VALUES);
      } else if (p.nvalues == 1) {
        Serial.printf("constant 0x%02X\n", p.values[0]);
      } else {
        for (uint8_t k = 0; k < p.nvalues; k++) {
          Serial.printf("0x%02X(%u) ", p.values[k], p.values[k]);
        }
        Serial.println("  <-- state?");
      }
    }
  }
  Serial.println("=================================================");
  Serial.println();
}

// ------------------------------------------------- plumbing

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 10: decoding an unknown payload ===");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_receive();

  Serial.printf("Listening on channel %d.\n", CHANNEL);
  Serial.println("A payload profile prints every 15 seconds.");
  Serial.println();
}

void loop() {
  while (q_tail != q_head) {
    RxFrame &f = queue[q_tail];
    Mac m;

    if (parseShortAddressed(f.data, f.len, &m) && m.type == 1) {
      learn(m, f.data);

      uint8_t id; bool on;
      if (decodeMonitor(m, f.data, &id, &on)) {
        Serial.printf("monitor %u: laser %-3s   (pan 0x%04X src 0x%04X seq 0x%02X rssi %d dBm)\n",
                      id, on ? "ON" : "off", m.pan, m.src, m.seq, f.rssi);
      } else {
        Serial.printf("unknown payload from 0x%04X:", m.src);
        for (uint8_t i = 0; i < m.payload_len; i++) {
          Serial.printf(" %02X", f.data[m.payload_off + i]);
        }
        Serial.println();
      }
    }

    q_tail = (q_tail + 1) % QUEUE_LEN;
  }

  static uint32_t last = 0;
  if (millis() - last > 15000) {
    last = millis();
    printProfile();
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

// -------------------------------------------------------------------------
// LOCK IT IN WITH A TEST
//
// A decode that lives only in a sketch rots. Copy two frames verbatim out of
// your capture, write the builder, and assert that it reproduces them byte for
// byte. laser_xbee_esp/test/test_frame.c in this repository does exactly that,
// and it runs on a laptop with no hardware:
//
//     gcc -Imain test/test_frame.c main/xbee_frame.c -o t && ./t
//
// The on-air confirmation is the one that counts, though: transmit your
// reconstructed frame at the real device and see whether it ACKs. An ACK means
// its MAC accepted every field in your header. That is chapter 9.
//
// -------------------------------------------------------------------------
// HONEST LIMITS OF THIS DECODE
//
// Monitors 2 and 3 never showed their "laser on" codes (21, 31) in any
// capture - those are inferred from the pattern, not observed. Write down
// which parts of a decode you have seen and which you have guessed, because
// the guesses are where the next bug lives.

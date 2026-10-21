// Chapter 7 - Reading the header
//
// Goal: stop staring at hex and let the sketch name the fields.
//
// Everything up to the payload is plain IEEE 802.15.4, identical for Zigbee,
// Thread and Digi's XBee 802.15.4 firmware. Parse it once and you can tell a
// data frame from an ACK, see who is talking to whom, and - most importantly -
// know exactly where the payload starts.
//
// The header is variable length. Three fields in the Frame Control Field
// decide its size, which is why guessing byte offsets does not work:
//
//   destination addressing mode   none / 16-bit / 64-bit
//   source addressing mode        none / 16-bit / 64-bit
//   PAN ID compression            is the source PAN ID present at all
//
// All multi-byte fields are little-endian. The bytes 61 88 on the wire are the
// 16-bit value 0x8861, and reading them in the wrong order is the single most
// common mistake when decoding these frames by hand.

#include <Arduino.h>
#include <string.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define CHANNEL       21
#define QUEUE_LEN     32
#define MAX_FRAME_LEN 128

struct RxFrame {
  uint8_t len;
  int8_t  rssi;
  uint8_t lqi;
  uint8_t data[MAX_FRAME_LEN];
};
static RxFrame queue[QUEUE_LEN];
static volatile uint8_t q_head = 0, q_tail = 0;

// ---------------------------------------------------------------- parsing

#define ADDR_NONE 0
#define ADDR_SHORT 2
#define ADDR_EXT   3

struct MacHeader {
  bool     valid;
  uint16_t fcf;
  uint8_t  type;          // 0 beacon, 1 data, 2 ack, 3 MAC command
  bool     security, pending, ack_req, pan_compress;
  uint8_t  version, dst_mode, src_mode;
  uint8_t  seq;
  bool     has_dst_pan, has_src_pan;
  uint16_t dst_pan, src_pan;
  uint16_t dst_short, src_short;   // valid when the matching mode is ADDR_SHORT
  uint8_t  dst_ext[8], src_ext[8]; // valid when the matching mode is ADDR_EXT
  uint8_t  payload_off;            // offset into the PSDU
  uint8_t  payload_len;
};

static const char *frameTypeName(uint8_t t) {
  switch (t) {
    case 0: return "Beacon";
    case 1: return "Data";
    case 2: return "Ack";
    case 3: return "MAC command";
    default: return "reserved";
  }
}

static const char *addrModeName(uint8_t m) {
  switch (m) {
    case ADDR_NONE:  return "none";
    case ADDR_SHORT: return "short";
    case ADDR_EXT:   return "extended";
    default: return "reserved";
  }
}

// psdu points at the PSDU (what the callback calls frame[1]); psdu_len is
// frame[0]. The last two bytes of a *received* PSDU are RSSI and LQI, not the
// FCS - see chapter 8 - so the MAC content ends two bytes early.
static bool parseMac(const uint8_t *psdu, uint8_t psdu_len, MacHeader *h) {
  memset(h, 0, sizeof(*h));
  if (psdu_len < 5) return false;          // 2 FCF + 1 seq + 2 trailing
  const uint8_t mac_len = psdu_len - 2;

  uint8_t off = 0;
  h->fcf = (uint16_t)psdu[0] | ((uint16_t)psdu[1] << 8);
  off = 2;

  h->type         = h->fcf & 0x07;
  h->security     = (h->fcf >> 3) & 1;
  h->pending      = (h->fcf >> 4) & 1;
  h->ack_req      = (h->fcf >> 5) & 1;
  h->pan_compress = (h->fcf >> 6) & 1;
  h->dst_mode     = (h->fcf >> 10) & 0x03;
  h->version      = (h->fcf >> 12) & 0x03;
  h->src_mode     = (h->fcf >> 14) & 0x03;

  if (off >= mac_len) return false;
  h->seq = psdu[off++];

  // Destination PAN ID is present whenever there is a destination address.
  if (h->dst_mode != ADDR_NONE) {
    if (off + 2 > mac_len) return false;
    h->dst_pan = (uint16_t)psdu[off] | ((uint16_t)psdu[off + 1] << 8);
    h->has_dst_pan = true;
    off += 2;
  }
  if (h->dst_mode == ADDR_SHORT) {
    if (off + 2 > mac_len) return false;
    h->dst_short = (uint16_t)psdu[off] | ((uint16_t)psdu[off + 1] << 8);
    off += 2;
  } else if (h->dst_mode == ADDR_EXT) {
    if (off + 8 > mac_len) return false;
    memcpy(h->dst_ext, psdu + off, 8);
    off += 8;
  }

  // Source PAN ID is omitted when PAN ID compression is set, because both
  // endpoints are in the same PAN. This is the field that most often throws
  // off a hand-written parser: the header is 2 bytes shorter than expected.
  if (h->src_mode != ADDR_NONE) {
    if (h->pan_compress) {
      h->src_pan = h->dst_pan;       // implied, not transmitted
      h->has_src_pan = false;
    } else {
      if (off + 2 > mac_len) return false;
      h->src_pan = (uint16_t)psdu[off] | ((uint16_t)psdu[off + 1] << 8);
      h->has_src_pan = true;
      off += 2;
    }
  }
  if (h->src_mode == ADDR_SHORT) {
    if (off + 2 > mac_len) return false;
    h->src_short = (uint16_t)psdu[off] | ((uint16_t)psdu[off + 1] << 8);
    off += 2;
  } else if (h->src_mode == ADDR_EXT) {
    if (off + 8 > mac_len) return false;
    memcpy(h->src_ext, psdu + off, 8);
    off += 8;
  }

  h->payload_off = off;
  h->payload_len = (off <= mac_len) ? (mac_len - off) : 0;
  h->valid = true;
  return true;
}

static void printExt(const uint8_t *a) {
  for (int i = 7; i >= 0; i--) {            // conventional big-endian display
    Serial.printf("%02X", a[i]);
    if (i) Serial.print(':');
  }
}

static void describe(const RxFrame &f) {
  MacHeader h;
  Serial.printf("len %2u  rssi %4d dBm  lqi %3u  |", f.len, f.rssi, f.lqi);
  for (uint8_t i = 0; i < f.len; i++) Serial.printf(" %02X", f.data[i]);
  Serial.println();

  if (!parseMac(f.data, f.len, &h)) {
    Serial.println("    (too short or malformed to parse)");
    Serial.println();
    return;
  }

  Serial.printf("    FCF 0x%04X  %s", h.fcf, frameTypeName(h.type));
  if (h.ack_req)      Serial.print(", ACK requested");
  if (h.pending)      Serial.print(", frame pending");
  if (h.security)     Serial.print(", SECURED");
  if (h.pan_compress) Serial.print(", PAN compressed");
  Serial.printf(", ver %u\n", h.version);

  Serial.printf("    seq 0x%02X (%u)\n", h.seq, h.seq);

  Serial.print("    dst  ");
  if (h.dst_mode == ADDR_NONE) {
    Serial.print("none");
  } else {
    Serial.printf("pan 0x%04X  ", h.dst_pan);
    if (h.dst_mode == ADDR_SHORT) {
      Serial.printf("addr 0x%04X%s", h.dst_short,
                    h.dst_short == 0xFFFF ? " (broadcast)" : "");
    } else {
      printExt(h.dst_ext);
    }
  }
  Serial.println();

  Serial.print("    src  ");
  if (h.src_mode == ADDR_NONE) {
    Serial.print("none");
  } else {
    Serial.printf("pan 0x%04X%s  ", h.src_pan, h.has_src_pan ? "" : " (implied)");
    if (h.src_mode == ADDR_SHORT) Serial.printf("addr 0x%04X", h.src_short);
    else                          printExt(h.src_ext);
  }
  Serial.printf("   [%s/%s addressing]\n",
                addrModeName(h.dst_mode), addrModeName(h.src_mode));

  Serial.printf("    payload %u byte%s at offset %u:",
                h.payload_len, h.payload_len == 1 ? "" : "s", h.payload_off);
  for (uint8_t i = 0; i < h.payload_len; i++) {
    Serial.printf(" %02X", f.data[h.payload_off + i]);
  }
  Serial.println();

  // Printable text in a payload is a strong hint about the protocol above the
  // MAC. All dots means it is binary, and you are in for chapter 10.
  if (h.payload_len) {
    Serial.print("    as text: \"");
    for (uint8_t i = 0; i < h.payload_len; i++) {
      char c = (char)f.data[h.payload_off + i];
      Serial.print((c >= 32 && c <= 126) ? c : '.');
    }
    Serial.println("\"");
  }
  Serial.println();
}

// ---------------------------------------------------------------- plumbing

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 7: header parsing ===");

  esp_ieee802154_enable();
  esp_ieee802154_set_promiscuous(true);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_receive();

  Serial.printf("Listening on channel %d\n\n", CHANNEL);
}

void loop() {
  while (q_tail != q_head) {
    describe(queue[q_tail]);
    q_tail = (q_tail + 1) % QUEUE_LEN;
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

// Worked example
//
//   61 88 46 01 0B 00 00 01 00 83 00 0B C1 0B
//
//   61 88        FCF = 0x8861: Data, ACK requested, PAN compressed,
//                short/short addressing
//   46           sequence number
//   01 0B        destination PAN 0x0B01
//   00 00        destination address 0x0000
//   01 00        source address 0x0001  (no source PAN: compression is on)
//   83 00 0B     3-byte payload
//   C1 0B        RSSI -63 dBm, LQI 11  <- not the FCS; see chapter 8
//
// Compare with a 5-byte frame:
//
//   02 00 51 C3 07
//
//   02 00        FCF = 0x0002: Ack, no addressing fields at all
//   51           sequence number, matching the data frame being acknowledged
//   C3 07        RSSI/LQI
//
// An ACK carries no addresses and no payload, which is why it is 5 bytes and
// why the parser reports "none" for both endpoints.

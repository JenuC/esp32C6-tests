// Chapter 9 - Transmitting, and getting acknowledged
//
// Goal: send a frame that a real 802.15.4 device will accept, and know for
// certain that it arrived.
//
// Receiving is forgiving: a frame you fail to parse is a frame you ignore.
// Transmitting is not. Every field in the header has to be right or the
// receiver's hardware filter drops your frame before any software sees it, and
// you get no error - just silence.
//
// The one honest signal that you got it right is an ACK. The receiving MAC
// sends it automatically, in hardware, only if the frame passed CRC and passed
// its address filter. An ACK means the other end accepted your frame as
// genuine. That single bit of feedback tests the whole header at once.
//
// TWO IMPORTANT DIFFERENCES FROM RECEIVING
//
// 1. buf[0] is the length *including* the 2-byte FCS, but you do not write the
//    FCS. The hardware appends a real CRC. So for 12 bytes of header and
//    payload you set buf[0] = 14 and write 12 bytes.
//
// 2. Do not enable promiscuous mode here. Promiscuous switches off address
//    filtering *and* automatic ACKs, so a promiscuous node never replies to
//    anyone and never gets replied to. A sniffer and a participant are
//    different roles.
//
// You need a second radio to ACK you. Run chapter 7 on another C6 - but note
// that a chapter 7 sniffer is promiscuous and so will NOT ACK; it will show
// you the frame on the air, which is enough to confirm the bytes are right.
// For a real ACK, point this at a device that is a genuine member of the PAN.

#include <Arduino.h>
#include <string.h>

extern "C" {
  #include "esp_ieee802154.h"
}

#define CHANNEL      21
#define PAN_ID       0x0B01
#define MY_ADDR      0x0002   // this board
#define DEST_ADDR    0x0000   // who we are talking to
#define SEND_EVERY_MS 1000

static uint8_t  seq = 0;
static uint32_t sent = 0, acked = 0, failed = 0;

static uint8_t *put16(uint8_t *p, uint16_t v) {
  *p++ = v & 0xFF;          // little-endian, like every field in this header
  *p++ = v >> 8;
  return p;
}

// Builds a data frame with a 3-byte payload. Returns the number of bytes
// written into buf, including the leading length byte.
static size_t buildFrame(uint8_t *buf, uint8_t sequence, const uint8_t *payload, uint8_t plen) {
  uint8_t *p = buf + 1;     // buf[0] is the length, filled in at the end

  // FCF 0x8861: Data, ACK requested, PAN ID compression on, short/short
  // addressing. Compression is what lets us omit the source PAN ID, because
  // both ends are in the same PAN.
  p = put16(p, 0x8861);

  *p++ = sequence;
  p = put16(p, PAN_ID);     // destination PAN
  p = put16(p, DEST_ADDR);  // destination short address
  p = put16(p, MY_ADDR);    // source short address, no source PAN

  memcpy(p, payload, plen);
  p += plen;

  // +2 for the FCS the hardware will append. Writing the payload length here
  // instead of the PSDU length is the classic off-by-two in this API.
  buf[0] = (uint8_t)((p - (buf + 1)) + 2);
  return (size_t)(p - buf);
}

void setup() {
  Serial.begin(115200);
  uint32_t deadline = millis() + 2000;
  while (!Serial && millis() < deadline) delay(10);
  delay(300);

  Serial.println();
  Serial.println("=== Chapter 9: transmit ===");

  esp_ieee802154_enable();

  // Participant configuration, not sniffer configuration. These three calls do
  // nothing in promiscuous mode, which is why chapters 3-8 could skip them.
  esp_ieee802154_set_channel(CHANNEL);
  esp_ieee802154_set_panid(PAN_ID);
  esp_ieee802154_set_short_address(MY_ADDR);
  esp_ieee802154_set_promiscuous(false);

  // Required to receive the ACK: the radio has to be back in RX by the time it
  // arrives, which is well under a millisecond after the frame goes out.
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_receive();

  Serial.printf("ch %d  pan 0x%04X  me 0x%04X  -> dst 0x%04X\n",
                CHANNEL, PAN_ID, MY_ADDR, DEST_ADDR);
  Serial.println();
}

void loop() {
  uint8_t frame[32];
  uint8_t payload[3] = { 0x00, 0x00, 0x2A };   // whatever your protocol says

  size_t n = buildFrame(frame, seq, payload, sizeof(payload));

  Serial.printf("TX seq 0x%02X  psdu_len %u  |", seq, frame[0]);
  for (size_t i = 1; i < n; i++) Serial.printf(" %02X", frame[i]);
  Serial.println();

  // Second argument is CCA: listen before transmitting and back off if the
  // channel is busy. true is the polite choice and the right default on a
  // shared band; false transmits immediately.
  esp_err_t err = esp_ieee802154_transmit(frame, true);
  if (err != ESP_OK) {
    // This is a local rejection - the frame never reached the air.
    Serial.printf("   transmit() refused the frame: 0x%X\n", err);
  } else {
    sent++;
  }

  seq++;

  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    Serial.printf("\n-- sent %lu, acked %lu, failed %lu\n\n", sent, acked, failed);
  }

  delay(SEND_EVERY_MS);
}

// Called when the frame has left the radio. `ack` is non-NULL only if we asked
// for an ACK and one came back in time.
extern "C" void esp_ieee802154_transmit_done(const uint8_t *frame,
                                             const uint8_t *ack,
                                             esp_ieee802154_frame_info_t *ack_frame_info) {
  if (ack) {
    acked++;
    Serial.printf("   ACKed (seq 0x%02X", ack[0] >= 3 ? ack[3] : 0);
    if (ack_frame_info) Serial.printf(", rssi %d dBm", ack_frame_info->rssi);
    Serial.println(")");

    // The ACK arrived in a receive buffer, so it has to be returned like any
    // other received frame. Leak these and you hit chapter 4's wall again,
    // just more slowly.
    esp_ieee802154_receive_handle_done((uint8_t *)ack);
  } else {
    Serial.println("   sent, no ACK (nobody at that address, or it did not hear us)");
  }
}

extern "C" void esp_ieee802154_transmit_failed(const uint8_t *frame,
                                               esp_ieee802154_tx_error_t error) {
  failed++;
  // The common ones: a CCA failure means the channel was busy every time it
  // tried; an ACK timeout means the frame went out and nothing answered.
  Serial.printf("   TX failed, error %d\n", (int)error);
}

// Debugging a frame nobody accepts
//
// Sniff your own transmission first. Put chapter 7 on a second board on the
// same channel. If your frame does not show up there, the problem is the radio
// configuration. If it shows up but the real device ignores it, the problem is
// in the header fields, and chapter 7's parser will print them back to you.
//
// Then check, in this order: channel, PAN ID, destination address, and whether
// you set buf[0] to the payload length instead of the PSDU length.

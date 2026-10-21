#include <Arduino.h>

extern "C" {
  #include "esp_ieee802154.h"
}

// ---- config ----
#define MONITOR_ID          1        // 1/2/3 -> source address and command family (10/11, 20/21, 30/31)
#define RF_CHANNEL          21       // XBee CH = 0x15
#define PAN_ID              0x0B01   // XBee ID
#define DEST_ADDR           0x0000   // XBee DH/DL = 0/0
#define TX_POWER_DBM        0        // XB24 PL=4 = 0 dBm

#define LASER_PIN           4
#define LASER_ACTIVE_HIGH   1
#define DEBOUNCE_MS         30
#define RESEND_MS           1000     // heartbeat; match the original .ino
#define MAX_TRIES           4
#define ACK_WAIT_MS         20

#define SNIFF_ONLY          0        // 1: promiscuous listen on RF_CHANNEL, no TX

#ifdef RGB_BUILTIN
#define LED_PIN             RGB_BUILTIN
#else
#define LED_PIN             8        // C6 DevKitC-1 RGB; C5 DevKitC-1 uses 27
#endif
#define LED_BRIGHT          40       // 0-255, onboard RGB is very bright
#define ACK_FLASH_MS        60

// ---- TX result from radio ISR ----
enum TxResult : uint8_t { TX_PENDING, TX_ACKED, TX_NO_ACK, TX_CCA_BUSY, TX_ERROR };
static volatile TxResult tx_result = TX_PENDING;

// ---- RX ring buffer (same as listener) ----
#define QUEUE_LEN     16
#define MAX_FRAME_LEN 128
struct RxFrame { uint8_t len; int8_t rssi; uint8_t data[MAX_FRAME_LEN]; };
static RxFrame rxq[QUEUE_LEN];
static volatile uint8_t q_head = 0, q_tail = 0;

static uint8_t seq_no, digi_ctr;
static int laser_state, laser_cand = -1;
static int force_state = -1;          // serial override: -1 = follow pin
static unsigned long t_change, t_sent;
static bool comm_ok = false;
static unsigned long flash_until;

static uint8_t laserCmd(int on) { return MONITOR_ID * 10 + (on ? 1 : 0); }

static void buildFrame(uint8_t *f, uint8_t cmd) {
  uint8_t *p = f + 1;
  *p++ = 0x61; *p++ = 0x88;                         // FCF: data, ACK req, PAN compress, short/short
  *p++ = seq_no++;
  *p++ = PAN_ID & 0xFF;     *p++ = PAN_ID >> 8;
  *p++ = DEST_ADDR & 0xFF;  *p++ = DEST_ADDR >> 8;
  *p++ = MONITOR_ID & 0xFF; *p++ = MONITOR_ID >> 8;
  *p++ = digi_ctr++;                                // Digi header (MM=0) counter
  *p++ = 0x00;
  *p++ = cmd;
  f[0] = (p - (f + 1)) + 2;                         // PSDU length incl. FCS (added by HW)
}

static TxResult transmitOnce(const uint8_t *f) {
  tx_result = TX_PENDING;
  if (esp_ieee802154_transmit(f, true) != ESP_OK) return TX_ERROR;
  unsigned long t0 = millis();
  while (tx_result == TX_PENDING && millis() - t0 < ACK_WAIT_MS) delayMicroseconds(200);
  return tx_result == TX_PENDING ? TX_ERROR : tx_result;
}

static bool sendCmd(uint8_t cmd) {
  uint8_t f[MAX_FRAME_LEN];
  buildFrame(f, cmd);
  for (int i = 1; i <= MAX_TRIES; i++) {
    TxResult r = transmitOnce(f);
    if (r == TX_ACKED) {
      Serial.printf("cmd %u acked (try %d)\n", cmd, i);
      comm_ok = true;
      flash_until = millis() + ACK_FLASH_MS;
      return true;
    }
    delay(2 + random(8));
  }
  Serial.printf("cmd %u: no ACK after %d tries\n", cmd, MAX_TRIES);
  comm_ok = false;
  return false;
}

static int readLaser() {
  if (force_state >= 0) return force_state;
  return digitalRead(LASER_PIN) == (LASER_ACTIVE_HIGH ? HIGH : LOW);
}

static void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '0' || c == '1') force_state = c - '0';
    else if (c == 'a') force_state = -1;
    else continue;
    Serial.printf("override: %s\n", force_state < 0 ? "off (pin)" : (force_state ? "ON" : "OFF"));
  }
}

// laser ON = red, OFF = green; white blip on each ACK; comm lost = state colour alternating with blue
static void updateLed() {
  unsigned long t = millis();
  uint8_t b = LED_BRIGHT, r = 0, g = 0, bl = 0;
  if (SNIFF_ONLY)                 { bl = b / 2; }
  else if (t < flash_until)       { r = g = bl = b; }
  else if (!comm_ok && (t / 250) & 1) { bl = b; }
  else if (laser_state)           { r = b; }
  else                            { g = b; }

  static uint32_t last = 0xFFFFFFFF;
  uint32_t c = (uint32_t)r << 16 | g << 8 | bl;
  if (c != last) { rgbLedWrite(LED_PIN, r, g, bl); last = c; }
}

static void drainQueue() {
  while (q_tail != q_head) {
    RxFrame &f = rxq[q_tail];
    Serial.printf("[%4d dBm] len %2u:", f.rssi, f.len);
    for (uint8_t i = 0; i < f.len; i++) Serial.printf(" %02X", f.data[i]);
    Serial.println();
    q_tail = (q_tail + 1) % QUEUE_LEN;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LASER_PIN, INPUT);              // INPUT_PULLUP if the signal is open-drain

  seq_no = esp_random();
  digi_ctr = esp_random();

  esp_ieee802154_enable();
  esp_ieee802154_set_channel(RF_CHANNEL);
  esp_ieee802154_set_txpower(TX_POWER_DBM);
  esp_ieee802154_set_panid(PAN_ID);
  esp_ieee802154_set_short_address(MONITOR_ID);
  esp_ieee802154_set_promiscuous(SNIFF_ONLY);
  esp_ieee802154_set_rx_when_idle(true);
  esp_ieee802154_receive();

  laser_state = readLaser();
  Serial.printf("monitor %d on ch %d, PAN 0x%04X%s\n", MONITOR_ID, RF_CHANNEL, PAN_ID,
                SNIFF_ONLY ? " (sniff only)" : "");
  Serial.println("serial: '1' force ON, '0' force OFF, 'a' follow pin");
}

void loop() {
  drainQueue();
  updateLed();
  if (SNIFF_ONLY) { delay(5); return; }

  handleSerial();
  int v = readLaser();
  unsigned long t = millis();
  if (v != laser_cand) { laser_cand = v; t_change = t; }

  bool changed = laser_cand != laser_state && t - t_change >= DEBOUNCE_MS;
  if (changed) laser_state = laser_cand;

  if (changed || t - t_sent >= RESEND_MS) {
    sendCmd(laserCmd(laser_state));
    t_sent = t;
  }
  updateLed();
  delay(5);
}

// ---- radio callbacks (ISR context: keep short) ----

extern "C" void esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                             esp_ieee802154_frame_info_t *ack_info) {
  if (ack) esp_ieee802154_receive_handle_done(ack);   // ACK frames come from the same RX pool
  tx_result = TX_ACKED;
}

extern "C" void esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t err) {
  tx_result = err == ESP_IEEE802154_TX_ERR_NO_ACK   ? TX_NO_ACK :
              err == ESP_IEEE802154_TX_ERR_CCA_BUSY ? TX_CCA_BUSY : TX_ERROR;
}

extern "C" void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info) {
  if (frame == NULL) return;
  uint8_t len = frame[0] > MAX_FRAME_LEN ? MAX_FRAME_LEN : frame[0];
  uint8_t next = (q_head + 1) % QUEUE_LEN;
  if (next != q_tail) {
    rxq[q_head].len = len;
    rxq[q_head].rssi = info ? info->rssi : 0;
    memcpy(rxq[q_head].data, &frame[1], len);
    q_head = next;
  }
  esp_ieee802154_receive_handle_done(frame);
}

#include "radio.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "radio";
static QueueHandle_t s_txq, s_rxq;

typedef struct {
    uint8_t buf[128];
    int8_t rssi;
} rx_item_t;

void radio_init(uint8_t channel, int8_t tx_dbm, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    s_txq = xQueueCreate(4, sizeof(radio_result_t));
    s_rxq = xQueueCreate(8, sizeof(rx_item_t));

    uint8_t eui[8];
    esp_read_mac(eui, ESP_MAC_IEEE802154);

    ESP_ERROR_CHECK(esp_ieee802154_enable());
    esp_ieee802154_set_channel(channel);
    esp_ieee802154_set_txpower(tx_dbm);
    esp_ieee802154_set_panid(pan);
    esp_ieee802154_set_short_address(short_addr);
    esp_ieee802154_set_extended_address(eui);
    esp_ieee802154_set_promiscuous(promiscuous);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_receive();
    ESP_LOGI(TAG, "ch %u pan 0x%04X addr 0x%04X %s", channel, pan, short_addr,
             promiscuous ? "(promiscuous)" : "");
}

radio_result_t radio_send(const uint8_t *frame, uint32_t ack_wait_ms)
{
    radio_result_t r;
    xQueueReset(s_txq);
    if (esp_ieee802154_transmit(frame, true) != ESP_OK)
        return RADIO_ERR;
    if (xQueueReceive(s_txq, &r, pdMS_TO_TICKS(ack_wait_ms)) != pdTRUE)
        return RADIO_TIMEOUT;
    return r;
}

bool radio_recv(uint8_t *out, uint32_t wait_ms, int8_t *rssi)
{
    rx_item_t it;
    if (xQueueReceive(s_rxq, &it, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
        return false;
    memcpy(out, it.buf, it.buf[0] + 1);
    if (rssi) *rssi = it.rssi;
    return true;
}

static void IRAM_ATTR post_tx(radio_result_t r)
{
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_txq, &r, &hp);
    portYIELD_FROM_ISR(hp);
}

// Driver callbacks (weak in esp_ieee802154, run in ISR context)

void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack,
                                            esp_ieee802154_frame_info_t *info)
{
    if (ack) esp_ieee802154_receive_handle_done(ack);
    post_tx(RADIO_ACKED);
}

void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t err)
{
    post_tx(err == ESP_IEEE802154_TX_ERR_NO_ACK  ? RADIO_NO_ACK :
            err == ESP_IEEE802154_TX_ERR_CCA_BUSY ? RADIO_CCA_BUSY : RADIO_ERR);
}

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    rx_item_t it;
    uint8_t n = frame[0] < sizeof(it.buf) - 1 ? frame[0] : sizeof(it.buf) - 1;
    memcpy(it.buf, frame, n + 1);
    it.buf[0] = n;
    it.rssi = info->rssi;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_rxq, &it, &hp);
    esp_ieee802154_receive_handle_done(frame);
    portYIELD_FROM_ISR(hp);
}

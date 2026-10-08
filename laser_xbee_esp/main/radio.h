#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { RADIO_ACKED, RADIO_NO_ACK, RADIO_CCA_BUSY, RADIO_ERR, RADIO_TIMEOUT } radio_result_t;

void radio_init(uint8_t channel, int8_t tx_dbm, uint16_t pan, uint16_t short_addr, bool promiscuous);
radio_result_t radio_send(const uint8_t *frame, uint32_t ack_wait_ms);
bool radio_recv(uint8_t *out, uint32_t wait_ms, int8_t *rssi);  // out[0] = length

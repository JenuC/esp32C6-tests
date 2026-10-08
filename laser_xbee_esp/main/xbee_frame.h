#pragma once
#include <stddef.h>
#include <stdint.h>

#define XBEE_FRAME_BUF 32

typedef struct {
    uint16_t pan;
    uint16_t src;
    uint16_t dst;
} xbee_link_t;

// Builds an ESP-IDF 802.15.4 TX buffer: buf[0] = PSDU length (incl. 2-byte FCS added by HW).
// MAC: FCF 0x8861 (data, ACK req, PAN compress, short/short); payload: digi_hdr, 0x00, cmd.
size_t xbee_build_frame(uint8_t *buf, const xbee_link_t *link,
                        uint8_t seq, uint8_t digi_hdr, uint8_t cmd);

static inline uint8_t xbee_cmd(uint8_t monitor_id, int laser_on)
{
    return (uint8_t)(monitor_id * 10 + (laser_on ? 1 : 0));
}

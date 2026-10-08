#include "xbee_frame.h"

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    *p++ = v & 0xFF;
    *p++ = v >> 8;
    return p;
}

size_t xbee_build_frame(uint8_t *buf, const xbee_link_t *link,
                        uint8_t seq, uint8_t digi_hdr, uint8_t cmd)
{
    uint8_t *p = buf + 1;
    p = put16(p, 0x8861);
    *p++ = seq;
    p = put16(p, link->pan);
    p = put16(p, link->dst);
    p = put16(p, link->src);
    *p++ = digi_hdr;
    *p++ = 0x00;
    *p++ = cmd;
    buf[0] = (uint8_t)(p - (buf + 1) + 2);
    return (size_t)(p - buf);
}

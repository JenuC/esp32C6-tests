#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xbee_frame.h"

static const uint8_t cap_on[]  = {0x61,0x88,0x46,0x01,0x0B,0x00,0x00,0x01,0x00,0x83,0x00,0x0B};
static const uint8_t cap_off[] = {0x61,0x88,0xCB,0x01,0x0B,0x00,0x00,0x01,0x00,0xF9,0x00,0x0A};

int main(void)
{
    xbee_link_t l = { .pan = 0x0B01, .src = 1, .dst = 0 };
    uint8_t b[XBEE_FRAME_BUF];

    size_t n = xbee_build_frame(b, &l, 0x46, 0x83, xbee_cmd(1, 1));
    assert(n == 13 && b[0] == 14 && memcmp(b + 1, cap_on, 12) == 0);

    xbee_build_frame(b, &l, 0xCB, 0xF9, xbee_cmd(1, 0));
    assert(memcmp(b + 1, cap_off, 12) == 0);

    assert(xbee_cmd(2, 1) == 0x15 && xbee_cmd(3, 0) == 0x1E);
    puts("frame tests pass");
    return 0;
}

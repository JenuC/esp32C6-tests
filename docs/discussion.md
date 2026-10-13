# The two listener sketches: how they work

Covers `esp32-c6-listen-HEX15/esp32-c6-listen-HEX15.ino` and
`esp32-c6-listener/esp32-c6-listener.ino`. Both run on a Seeed XIAO ESP32-C6 and
both do the same job — put the 802.15.4 radio into promiscuous receive and print
every frame that lands — but they sit at different points in the project's
history, and the differences between them are the lessons learned.

HEX15 is the first attempt: one fixed channel, print straight from the radio
callback. The listener is the second: sweep channels 11–26, hand frames to
`loop()` through a queue, and give the driver its buffers back. Read HEX15 first
to see the shape of the problem, then the listener to see what had to change.

## 1. What the radio hands you

The ESP32-C6 has a hardware IEEE 802.15.4 PHY/MAC alongside its Wi-Fi and BLE
radios. It is the same radio layer Zigbee, Thread and Digi's XB24 802.15.4
firmware sit on top of. Channels 11–26 are 5 MHz apart starting at 2405 MHz, so
channel *n* is at `2405 + 5 × (n − 11)` MHz, overlapping the 2.4 GHz Wi-Fi band.

When a frame passes CRC, the driver calls:

```c
void esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info)
```

`frame` points into a driver-owned buffer laid out as:

```
frame[0]        PSDU length, counting the 2-byte FCS field
frame[1..len]   the PSDU itself: MAC header, payload, then 2 trailing bytes
```

Two things about that layout bite people, and both are visible in the capture
logs under `esp32-c6-listener/esp-listener-output/`:

- `frame[0]` is a length *including* the FCS field, so a frame printed as
  "14 bytes" carries 12 bytes of MAC header and payload.
- Those last 2 bytes are not the CRC by the time you see them. The hardware has
  already checked and consumed the CRC, and the receiver overwrites that space
  with its own measurements: RSSI, then a small link-quality value. The evidence
  is in [decoding.md](decoding.md) — the same 12-byte frame body shows up in the
  logs with five different trailing pairs. Read `frame_info->rssi` and
  `frame_info->lqi` if you want those numbers properly; don't parse them out of
  the tail.

`esp_ieee802154_set_promiscuous(true)` is what makes a listener possible. It
turns off hardware address filtering, so frames for any PAN ID and any
destination are delivered instead of being dropped. A side effect worth knowing:
once promiscuous is on, `esp_ieee802154_set_panid()` and `set_short_address()`
no longer affect what you receive. HEX15 sets a PAN ID anyway (line 20). It is
harmless, and it is also doing nothing.

## 2. Principles of operation

These five rules are what the two sketches are really about.

**The callback runs in driver/ISR context.** It is not a task. Anything slow in
there stalls the radio and costs you frames. A 14-byte hex dump at 115200 baud
is roughly 42 characters, about 3.6 ms of blocking — longer than the air time of
the frame that triggered it. HEX15 prints hex *and* ASCII from inside the
callback and will drop frames during any burst.

**Buffers are a pool, and you must return them.** The pointer you get is one of
`CONFIG_IEEE802154_RX_BUFFER_SIZE` statically allocated buffers (default 20). It
stays checked out to your code until you call
`esp_ieee802154_receive_handle_done(frame)`. Skip that call and the radio stops
delivering frames permanently once the twentieth one arrives — not on one
channel, on all of them, because the pool is global. `output.txt` in the capture
directory ends after exactly 20 frames for this reason. The longer logs in that
directory necessarily come from a build that returns its buffers.

**Copy out, parse later.** The buffer is only yours until you release it, so the
callback's job is to memcpy what it needs into your own storage and get out. The
listener copies into a ring buffer and lets `loop()` do the printing; the queue
is the whole reason it can hold a channel open for two seconds without losing
anything.

**Re-arming is per channel, not per tick.** Both sketches originally called
`esp_ieee802154_receive()` on a timer — every 100 ms in HEX15, every 50 ms in the
first version of the listener. That is unnecessary. With
`set_rx_when_idle(true)` the radio stays in RX by itself; you arm it once after a
channel change and leave it alone.

**A scanner is blind most of the time.** Sixteen channels at 2 s each is a 32 s
sweep, so any one channel is watched 6.25% of the time. That is fine for the
monitors here, which beacon about once a second — two seconds of dwell catches
one or two of their heartbeats. It is useless for catching a one-shot event. If
you care about a specific channel, pin the radio to it the way HEX15 does.

## 3. `esp32-c6-listen-HEX15.ino` — fixed channel

```
setup()   enable radio -> set channel -> set PAN -> promiscuous -> receive()
loop()    re-arm receive(), sleep 100 ms, forever
callback  print length, hex dump, ASCII rendering
```

The useful thing about this sketch is the ASCII column. When you have no idea
what a protocol is, printing the payload as text tells you immediately whether
there are strings in it. For these monitors there are none — the payload is
three binary bytes — so the ASCII column stayed empty and the listener dropped
it.

Three defects, all worth knowing if you go back to this file:

- **It does not release buffers.** No `receive_handle_done()`, so it dies after
  20 frames.
- **It prints from the callback.** See above.
- **The channel comments are wrong.** Line 17 calls
  `esp_ieee802154_set_channel(16)` under a comment saying "Set Channel to 15
  (0x0F)", and line 28 prints "Listening on Channel 15 (0x0F)". The sketch
  listens on channel 16. The name "HEX15" refers to 0x15 = 21 decimal, which is
  a third channel again, and the one the monitors in 159A actually use. If you
  are matching logs to channels, trust the `set_channel()` argument and nothing
  else in this file.

## 4. `esp32-c6-listener.ino` — sweeping scanner

```
setup()      enable -> promiscuous -> rx_when_idle -> set channel 11 -> receive()
loop()       announce channel, drain queue for 2 s, hop, set channel, receive()
callback     bounds-check length, memcpy into ring buffer, release buffer
drainQueue() print whatever the callback left behind
```

The ring buffer is the only structural difference that matters, so it is worth
being precise about why it is safe. It is a single-producer, single-consumer
queue: the callback only ever writes `q_head`, `loop()` only ever writes
`q_tail`, and neither touches the other's index. That is what lets it work
without a lock or a critical section. Both indices are `volatile` so the
compiler cannot cache them across the loop test.

Two deliberate choices in there:

- **Full means drop.** The callback checks `next != q_tail` and discards the
  frame if the queue is full, rather than overwriting the oldest. Losing the
  newest frame is recoverable; a torn frame that `drainQueue()` is halfway
  through printing is not.
- **Length is clamped** to `MAX_FRAME_LEN` before the memcpy. `frame[0]` comes
  off the air, and a corrupt value would otherwise overrun `RxFrame.data`.
  802.15.4 caps the PSDU at 127 bytes, so 128 is the real maximum and the clamp
  never fires in normal use.

`esp_ieee802154_receive_handle_done(frame)` is the last line of the callback,
after the copy. It needs ESP-IDF 5.1 or later, which means Arduino-ESP32 core
3.x. On an older core the function does not exist and there is no way to return a
buffer; upgrade rather than working around it.

The channel hop at the end of `loop()` sets the channel and re-arms in that
order. One cosmetic consequence: a frame still sitting in the queue when the hop
happens gets printed after the next channel's banner. The channel number on the
frame itself is right, because the callback stamps `f.channel` at receive time —
only the position relative to the banner is misleading. Expect the occasional
frame to appear just below a banner it does not belong to.

## 5. API used

| Call | Purpose |
|---|---|
| `esp_ieee802154_enable()` | power up the 802.15.4 subsystem |
| `esp_ieee802154_set_channel(n)` | tune to channel 11–26 |
| `esp_ieee802154_set_promiscuous(true)` | deliver all frames, bypass address filters |
| `esp_ieee802154_set_rx_when_idle(true)` | return to RX automatically instead of idling |
| `esp_ieee802154_receive()` | enter receive state |
| `esp_ieee802154_receive_handle_done(frame)` | return a buffer to the RX pool (required) |
| `esp_ieee802154_set_panid()` / `set_short_address()` | address filtering; no effect while promiscuous |
| `esp_ieee802154_transmit(buf, cca)` | send a raw PSDU, `buf[0]` = length |

## 6. Where this went next

The listener's output is what made `laser_xbee_esp/` possible: once the frame
format was known, an ESP32-C6 could impersonate the XB24 transmitter and talk to
the existing indicator box directly. `laser_xbee_esp/main/config.h` has a
`SNIFF_MODE` flag that reuses the listener's approach inside that project, so you
can confirm you are seeing the real monitor before you replace it.

[decoding.md](decoding.md) covers how the frame format was worked out from these
logs.

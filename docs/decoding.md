# Decoding the monitor frames

How the hex dumps from `esp32-c6-listener` turned into the frame builder in
`laser_xbee_esp/main/xbee_frame.c`. Everything below is derived from the logs in
`esp32-c6-listener/esp-listener-output/`, which hold about 12,800 frames across
six channels.

The target: Digi XB24 radios running 802.15.4 firmware 10EF, each wired to a
laser-safety monitor, beaconing status to an indicator box roughly once a second.
No documentation of the payload existed. The only inputs were Brandon's
spreadsheet of channel and PAN assignments (copied into `outputs.txt`) and
whatever the radios put on the air.

## 1. Sorting the captures by length

The first pass over a log shows only two frame sizes, and the split is almost
even:

| PSDU length | Count | FCF bytes |
|---|---|---|
| 14 | 6,426 | `61 88` |
| 5 | 6,412 | `02 00` |

Two sizes with matching counts is the signature of an acknowledged unicast
stream, so the 5-byte frames were set aside as ACKs and confirmed later
(§6). That left one frame format to crack.

One trap when counting these yourself: the long `...off.txt` log also contains
2,430 `TX Success! Seq: 0x.. | Length: 14 bytes` lines, which are
`esp32-c6-room159A`'s own transmit prints, not received frames. Match on the
`DETECTED TRAFFIC ... Hex:` prefix, not on `Length: 14`. Nothing that sketch sent
appears in the receive data — it uses source `0x005D`, and the only sources ever
received are `0x0001`, `0x0002` and `0x0003`.

## 2. Splitting the 14-byte frame

802.15.4 fixes the header, so the first job is just applying the standard. The
Frame Control Field is little-endian, so `61 88` on the wire reads as **0x8861**:

| FCF bits | Value | Meaning |
|---|---|---|
| 0–2 frame type | `001` | Data |
| 3 security | `0` | off |
| 4 pending | `0` | no more data |
| 5 ACK request | `1` | sender wants an ACK |
| 6 PAN ID compression | `1` | one PAN ID present, source PAN omitted |
| 10–11 dest addr mode | `10` | 16-bit short |
| 14–15 src addr mode | `10` | 16-bit short |

PAN ID compression plus two short addresses pins the header down to 7 bytes
after the FCF, which splits the frame cleanly:

```
61 88 | 46 | 01 0B | 00 00 | 01 00 | 83 00 0B | C1 0B
  |      |     |        |       |        |         |
  FCF   seq   PAN     dest    source  payload   RSSI/LQI
             0x0B01  0x0000   0x0001  (3 bytes)
```

All multi-byte fields are little-endian. The destination was `0x0000` in all
6,426 data frames, matching an XBee configured `DH=0, DL=0` — the indicator box
is address 0.

That leaves one unknown of interest: the 3-byte payload.

## 3. The trailing 2 bytes are not the CRC

Worth getting out of the way first, because it shortens the payload from five
bytes to three.

The obvious reading of a 14-byte PSDU is 12 bytes of MAC plus a 2-byte FCS. That
is correct on the air. It is wrong in the receive buffer, and the logs prove it:
group every frame by its first 12 bytes and look at what follows.

```
body 61 88 69 01 0B 00 00 02 00 B6 00 14
  trailers seen: BA 06, BA 09, BA 0A, BB 09, BB 0A
```

315 distinct frame bodies appear with more than one trailing pair. A CRC is a
function of the frame content, so identical content cannot produce five
different checksums. These bytes are written by the receiver, not the sender.

Reading the first one as a signed int8 settles what they are:

| Channel | Frames | Mean byte[12] | Range |
|---|---|---|---|
| 14 | 397 | −94.7 | −106 … −86 |
| 16 | 847 | −90.2 | −99 … −84 |
| 17 | 717 | −95.1 | −108 … −86 |
| 18 | 544 | −88.4 | −97 … −83 |
| 21 | 3,026 | −62.2 | −76 … −49 |
| 25 | 895 | −75.4 | −95 … −67 |

Those are dBm. Channel 21 is the room the ESP32 was sitting in and reads about
30 dB hotter than the distant rooms. The second byte stays in the range 1–11 and
behaves like a link-quality or correlation count. So the ESP32-C6 hardware
overwrites the FCS field with RSSI and LQI once it has verified the CRC, which
is why the listener's dump shows them.

Consequence for anyone reading these logs: **the payload is 3 bytes, not 5.**
`frame_info->rssi` and `frame_info->lqi` are the supported way to get the same
numbers.

## 4. The payload: an A/B experiment

Three captures were taken under known conditions, which is what made the payload
readable. Grouping the payload's last byte by source address:

| Capture | src 0x0001 | src 0x0002 | src 0x0003 |
|---|---|---|---|
| `...21-monito10-11 off.txt` | `0A` (10) | `14` (20) | `1E` (30) |
| `...21-monito10-11 on.txt` | `0A` (10) | `14` (20) | `1E` (30) |
| `...21-monito10-11 on laseron.txt` | **`0B` (11)** | `14` (20) | `1E` (30) |

The byte takes exactly one value per source in every capture, and only monitor 1
changes — in the one capture where monitor 1's laser was on. That is the whole
protocol:

```
cmd = monitor_id × 10 + (laser_on ? 1 : 0)
```

So 10/11 for monitor 1, 20/21 for monitor 2, 30/31 for monitor 3. The values are
decimal-looking in hex (`0x0A` = 10, `0x14` = 20, `0x1E` = 30), which reads like
someone picking numbers for a human rather than packing bits. The source short
address doubles as the monitor ID, straight from the XBee's `MY` parameter.

This is `xbee_cmd()` in `laser_xbee_esp/main/xbee_frame.h`:

```c
return (uint8_t)(monitor_id * 10 + (laser_on ? 1 : 0));
```

Monitors 2 and 3 never showed their "on" codes (21, 31) in any capture, so those
are inferred from the pattern rather than observed. Nothing in the project
depends on them yet.

The middle payload byte was `0x00` in all 6,426 frames and is carried as a
literal.

## 5. The first payload byte: a second counter

The byte at offset 9 varies frame to frame and was the last thing to make sense.
It is not random and it is not part of the status.

Taking consecutive frames from the same source and comparing how the MAC sequence
number and this byte move together:

| Channel / source | Frames | (ΔSeq, Δbyte9) = (1,1) | ΔSeq == Δbyte9 |
|---|---|---|---|
| ch 14 src 1 | 363 | 261 | 93.4% |
| ch 16 src 1 | 823 | 704 | 96.0% |
| ch 17 src 1 | 690 | 529 | 95.4% |
| ch 21 src 1 | 547 | 496 | 96.3% |
| ch 21 src 2 | 1,598 | 839 | 96.4% |
| ch 25 src 1 | 862 | 810 | 96.2% |

The two counters advance in lockstep about 95% of the time, from different
starting points, with the offset slipping by one every few hundred frames. That
is a second sequence counter maintained one layer above the MAC — Digi's own
frame counter — rather than anything derived from the MAC sequence number.

Because the offset drifts, it cannot be computed from `seq`. The frame builder
takes it as a caller-supplied parameter (`digi_hdr`) and `main.c` runs its own
counter, which is behaviourally identical to what the real radios do.

The remaining ~5% of pairs are mostly `(0,0)`: the exact same frame seen twice.
Those are MAC retransmissions, and on channel 21 source 2 they account for 692 of
1,597 consecutive pairs — that radio was getting a lot of ACKs lost. They are
also where §3's identical-body-different-trailer evidence comes from.

## 6. Confirming the 5-byte frames

`02 00` as an FCF is 0x0002: frame type `010` = Acknowledgment, no addressing
fields, no payload. The format is forced — 2 FCF, 1 sequence number, 2 bytes that
are RSSI/LQI by §3.

```
02 00 51 C3 07
 |    |    |
 FCF  seq  RSSI/LQI
```

The sequence numbers match data frames seen moments earlier on the same channel,
which is what an ACK is. Their presence also confirms the ACK-request bit read in
§2, and gives a free health check on the link: a data frame with no following ACK
got lost.

## 7. Channel and PAN map

Cross-tabulating PAN ID against channel across every log gives a clean one-to-one
mapping with no cross-channel leakage:

| Channel | Hex | PAN ID | Room (from `outputs.txt`) |
|---|---|---|---|
| 14 | 0x0E | 0x0E01 | 157 |
| 16 | 0x10 | 0x1001 | 163 |
| 17 | 0x11 | 0x1101 | 167 |
| 18 | 0x12 | 0x1201 | not in spreadsheet |
| 21 | 0x15 | 0x0B01 | 159A |
| 25 | 0x19 | 0x0D01 | 159B |

Four of the six follow a convention of `PAN = (channel in hex) << 8 | 0x01`,
which makes a PAN ID guessable from a channel. 159A and 159B do not — they use
0x0B01 and 0x0D01, presumably commissioned before the convention or just set by
hand. Don't assume the pattern holds for a room you have not sniffed.

Two loose ends from the sweep:

- **Channel 15 (room 158) was silent** in every capture, though the spreadsheet
  lists it. Either that monitor is off, retuned, or out of range.
- **Channel 18 was not in the spreadsheet** but carries a monitor with the
  conventional PAN 0x1201.

## 8. The result

```
61 88 | seq | 01 0B | 00 00 | id 00 | digi | 00 cmd | FCS (hardware)
```

In `laser_xbee_esp/main/xbee_frame.c`, with `buf[0]` set to 14 — twelve bytes
written plus the 2-byte FCS the hardware appends. Note the asymmetry with
receive: on transmit those last two bytes really are the FCS.

`laser_xbee_esp/test/test_frame.c` locks this in by rebuilding two frames copied
verbatim out of the captures and asserting the bytes match:

```c
static const uint8_t cap_on[]  = {0x61,0x88,0x46,0x01,0x0B,0x00,0x00,0x01,0x00,0x83,0x00,0x0B};
static const uint8_t cap_off[] = {0x61,0x88,0xCB,0x01,0x0B,0x00,0x00,0x01,0x00,0xF9,0x00,0x0A};
```

Run it on a host, no hardware needed:

```bash
gcc -Imain test/test_frame.c main/xbee_frame.c -o t && ./t
```

The on-air confirmation is the one that counts, and `laser_xbee_esp/README.md`
has the procedure: sniff the real monitor first, then unplug it and check the log
says `cmd 11 acked`. An ACK coming back means the indicator's XBee accepted the
frame as genuine, which tests every field in the header at once.

## Method notes

What made this tractable, roughly in order of how much it helped:

1. **A/B captures under known physical conditions.** One capture with the laser
   on was worth more than all 12,000 passive frames. §4 falls out in one table;
   without it the status byte is just a number that sometimes changes.
2. **Counting before reading.** Grouping by length and by source, and checking
   which fields are constant across thousands of frames, separates structure from
   content before any byte gets interpreted. The all-zero destination and the
   constant middle payload byte came from this.
3. **Grouping by content to test an assumption.** The CRC question in §3 looked
   settled by the spec. One `group by first 12 bytes` disproved it, and that
   corrected the payload length the rest of the work depended on.
4. **Applying the standard first.** Everything down to the payload boundary is
   just 802.15.4, and PAN ID compression is the bit that decides where the
   payload starts. Guessing offsets without reading the FCF wastes time.
5. **A replay test against captured bytes.** `test_frame.c` makes the conclusion
   executable, so a later refactor cannot quietly break the format.

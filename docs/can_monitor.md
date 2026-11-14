# CAN monitor

The CAN monitor streams the frames of a CAN port to a client, with their receive times, over the BLE
access point's `bus` characteristic. A client (the configurator app) opens a **session** on a port with the
`canmon` SMP group (74, see [smp.md](smp.md#canmon-can-monitor-sessions)), reads the notifications, and, on
a build with transmit, sends frames, periodic frames and a replay of a capture on the same port. It watches a
channel the unit runs, a port the unit does not use, or a synthetic source that measures the link. It is
built only where a unit enables it: rt_gauge does, rt_daq does not (the EL PM100 has no Bluetooth). The domain
does not depend on a transport, so another link can carry the same group and records.

Code: `src/domain/can_monitor_domain/` (`services/` the sessions, the sender and the synthetic source,
`utilities/` the records, the ring and the bit times, `transmit/` the cap, the reserved IDs and the replay
buffer, `smp/` the group), `src/subsys/canbus/` (receive times, the tap, the echo of the unit's own frames),
`src/subsys/bluetooth/access_point/` (the `bus` characteristic and the link hold),
`src/controllers/can_monitor_controller.*`. The group's CBOR is in
`src/configuration/cbor/cbor_schemas/can_monitor_smp.cddl`. The subsystem and link parts are in [canbus.md](canbus.md#the-tap)
and [bluetooth.md](bluetooth.md#gatt-service); this page describes the sessions and what goes over the wire.

Every wire change is additive: a new group, a new characteristic appended last, and Kconfig symbols that
default to off. The monitor stores nothing: it has no stored configuration, and no CDMP record or existing SMP
answer changed.

## Sessions

| Mode | Value | For | Bus |
| --- | --- | --- | --- |
| `TAP` | 0 | a channel the stored configuration runs | The channel's own bus, as it runs: its mode, bitrate and consumers stay as they are; the session adds a [tap](canbus.md#the-tap). OPEN's `type` and bitrates are ignored; the answer states the channel's. |
| `LISTEN_ONLY` | 1 | a port the configuration does not use | A `Canbus` the session builds on the port with `CAN_MODE_LISTENONLY`: it receives and never acknowledges or sends. |
| `ACTIVE` | 2 | a port the configuration does not use | The same, in normal mode: it acknowledges frames and may transmit (see [Transmit and replay](#transmit-and-replay)). |
| synthetic | | no port | No bus: generated frames, on port 255 (OPEN's `synthetic`; `EERIE_LEAP_CAN_MONITOR_SYNTHETIC`). |

A mode the port cannot take is `BAD_MODE`: `TAP` on a port the configuration does not run, `LISTEN_ONLY` or
`ACTIVE` on a channel it runs, a mode above 2, or CAN FD asked of a controller without it.

**A bus of its own.** OPEN gives the port's `type` (1 classical, the default; 2 CAN FD), `bitrate` and, on
CAN FD, `data_bitrate`. A bitrate of 0 detects it (the bus listens and tries the supported bitrates, see
[canbus.md](canbus.md#subsystem)): the answer says 0, no frame comes before the bitrate is found, and the first
`state` record carries it. Only the nominal bitrate of a CAN FD port can be detected; a data bitrate not given
is the nominal one. The bus is stopped and freed when the session ends. If it stops by itself the session ends
with the reason `error`; the lease task looks at it every 500 ms.

**STATUS** lists each `can<N>` alias the devicetree has (ports 0 to 7): the types it can run (1, and 2 when the
controller does CAN FD), whether the stored configuration runs it (`configured`), its state, its bitrates (the
detected ones, 0 until detected) and whether a session holds it. It also gives `filters_max`, `ring_bytes`,
`notify_max` and what the build has: `tx`, `synthetic` and `delta`.

**Tokens and the lease.** OPEN answers a `tok` (a random `uint32`, never 0) and `lease_ms`
(`EERIE_LEAP_CAN_MONITOR_LEASE_MS`, 5 s). Every request that carries the token renews the lease: FILTER, STATS
and, with transmit, ARM, SEND, PERIODIC and REPLAY. The stream does not. A client that streams and sends
nothing polls STATS, which also gives the totals. When the lease lapses the session ends, with the reason
`lease`; a client that is gone frees its port that way.

**A lost OPEN answer.** The same OPEN (port, mode, type, bitrates, synthetic) repeated before any request has
carried the token gets the same answer and token rather than `BUSY`, as the `ui` group's BEGIN does. The
repeat's `filters` and `delta` are not applied; send FILTER.

**Limits.** At most `EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS` (2) sessions at once, at most one a port; the
synthetic source is port 255. OPEN is refused with `BUSY` when the port has a session or no slot is free, and
with `NOT_LISTENING` while the central has not enabled notifications on `bus`. All sessions share the one BLE
connection.

**How a session ends.** The producer stops first (the tap removed, the own bus stopped, the source joined);
then, while the link is up, the sender drains the ring, adds a last `statistics` record and the `end` record,
and frees the session. The session keeps the fast link until its `end` record is built. A ring still not
drained after 5 s is cut: what it holds is dropped, its frames counted lost to the ring, and one `gap` record a
cause (the cut frames, and the gap records cut with them) goes ahead of the last `statistics` and the `end`
record; a link that does not take even those within another second frees the session without them. The port
is free for a new OPEN at once; a client that reopens the same port waits for the old session's `end` record
before it reads the new stream (the header carries no token).

| Cause | `end` reason | Record |
| --- | --- | --- |
| CLOSE | 1 closed | yes, after what the ring held |
| The lease lapsed | 2 lease | yes |
| The connection ended | 3 disconnected | never sent: there is no link; the session is freed at once |
| A CAN configuration was stored | 4 configuration | yes |
| The session's own bus stopped | 5 error | yes |

A session that ends while `bus` notifications are off is freed at once, with no records. A client that turns
them off while its session lives gets nothing: the ring fills, `gap` records follow once they are on again,
and the session ends only through its lease.

## The stream

A notification on `bus` (every multi-byte value little-endian):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `version` (1) |
| 1 | 1 | the port (`ch`; 255 for a synthetic session) |
| 2 | 2 | `seq`: 0 in the session's first notification, +1 for each notification sent (wraps) |
| 4 | 8 | `base_us`: the time of the record before this notification's first (the OPEN's time for the first notification) |
| 12 | 1 | `n`, the records that follow (at most 255) |

A notification holds whole records only; a record never spans two.

### Records

| Part | Size | Content |
| --- | --- | --- |
| head | 1 | DLC code (bits 0-3), IDE (4), FDF (5), BRS (6), ext (7: a second byte follows) |
| ext | 0 or 1 | RTR (bit 0), ESI (1), TX (2), TX failed (3), kind (4-7) |
| dt | 1-10 | µs since the previous record, unsigned LEB128 |
| body | by kind | below |

A record without the ext byte is a received data frame (kind 0), so most records have none. Every other
record starts with head `0x80` and the ext byte; its head carries no frame bits. TX marks a frame the unit sent
(seen when the controller completed it); TX failed says the driver reported an error for it, so it did not go.

| Kind | Body |
| --- | --- |
| 0 frame | ID (2 bytes with IDE clear, 4 with it set), then the payload |
| 1 changed-bytes frame | ID, a mask byte for each 8 payload bytes, then the bytes the masks name ([Changed bytes](#changed-bytes)) |
| 2 state | state (0 error active, 1 warning, 2 passive, 3 bus-off, 4 stopped), TEC, REC, then the detected bitrate as LEB128 (0 when unchanged) |
| 3 statistics | 13 LEB128s ([Statistics and bus state](#statistics-and-bus-state)) |
| 4 gap | frames lost (LEB128), the cause (below) |
| 5 end | the reason (1 closed, 2 lease, 4 configuration, 5 error; 3 is defined and never sent) |
| 6 tx event | the event (1 disarmed, 2 bus-off, 3 frames skipped, 4 replay done, 5 replay underrun), a count (LEB128; [Transmit and replay](#transmit-and-replay)) |
| 7 tap | 1 the filter swap may have repeated frames around it, 2 it may have missed some |

The DLC code is the CAN one: 0-8 are that many bytes, 9-15 are 12, 16, 20, 24, 32, 48 and 64 on an FDF frame (8
on a classical one). A remote frame (RTR) has the DLC it asked for and no payload. A classical 8-byte frame
with an 11-bit ID takes 12 bytes, or 13 with a dt of 128 µs or more; 14 or 15 with a 29-bit ID; a 64-byte FD
frame about 69.

### Times

A record's time is its dt added to the time of the record before it, and a notification's first record counts
from `base_us`. The time is µs since power-on, the base of `TIME`'s `uptime_ms` (see [smp.md](smp.md)), so a
client can put the stream on the unit's clock as it puts live values and logs.

- The records the sender adds (`statistics`, `tx event`, `end`) have dt 0 and leave the time where it was. A `gap` record
  has dt 0 too: the loss lies between the record before it and the one after.
- A record stamped before the one preceding it (the completion of a sent frame can arrive after a later
  received one) gets dt 0, so the stream's time never goes back.
- `base_us` chains across a notification the link dropped or a record the sender skipped: the time they would
  have added is kept.

### Gaps

Frames can be lost at four stages. Each is counted, and written as a `gap` record, before the next record that
finds room, with the number of frames and the cause:

| Cause | Code | Meaning |
| --- | --- | --- |
| Ring | 1 | The session's ring was full: the link or the sender could not keep up. |
| Receive queue | 2 | The bus's receive queue overflowed before the tap saw the frames (`EERIE_LEAP_CANBUS_RX_QUEUE_SIZE`). |
| Controller | 3 | The CAN controller overran its receive buffer (needs `CONFIG_CAN_STATS`, see below). Zephyr counts an overrun once, however many frames it dropped, so the gap is a floor. |
| Echo pool | 4 | The unit sent frames with no echo (no free `EERIE_LEAP_CAN_MONITOR_TX_SLOTS` slot, or the queue was full). The frames went out; they are missing from the stream. |

A notification the link refuses for good (any error but `-ENOMEM` and `-ENOTCONN`) is dropped: its frames are
counted as lost in the ring, no `gap` record is written, and `seq` shows the hole. Frames the unit filter leaves
out are not lost; they are counted as `filtered`.

### The `tap` record

A TAP session swaps the bus's hardware filters to receive every frame (see [the filter swap](canbus.md#the-tap)).
Once the swap is done, the first record after it is a `tap` record. While both filter sets are installed, a
frame that a unit consumer's filter also matches may be queued twice on the two drivers that call every matching
filter (the ESP32's TWAI and the loopback driver): mark 1, frames around the record may be **repeated**. The
MCP2518FD routes a frame through the first match and never repeats one. Where the driver has no room for both
sets the swap removes first and adds after, so frames may be **missed**: mark 2. A client does not trust the
frames right around a `tap` record for exact counts, and a changed-bytes table is reset by it. A bus that is
detecting its bitrate writes none, and none is written at the end of a session.

### Changed bytes

Built with `EERIE_LEAP_CAN_MONITOR_DELTA` (STATUS `delta`) and asked for by OPEN's `delta: true`; otherwise
every frame goes whole. The session and the client keep the same table and apply the same rules:

- **The table** is keyed by (ID, IDE) and holds the last payload (1 to 64 bytes) of up to
  `EERIE_LEAP_CAN_MONITOR_DELTA_IDS` identifiers. It is updated for every frame written, the unit's own
  included. Frames the unit filter leaves out never reach it.
- **Whole or changed.** A data frame goes as changed bytes (kind 1) when its key has an entry of the same
  payload length and the changed form is smaller than the whole record; otherwise whole (kind 0). Then the
  entry takes the frame's payload.
- **Insertion.** An entry is added while there is room and never evicted, so an ID that finds the table full
  goes whole until the next reset. Remote frames and empty payloads go whole and leave the table alone.
- **Reset.** A `gap` record and a `tap` record empty the table: the next frame of every ID goes whole. A frame
  lost to a full ring empties it at once, so the frame after the gap is whole. The client empties its table at
  every `gap` and `tap` record it reads. State, statistics, end and tx records do not reset it.
- **Layout.** Head and ext as a frame's (the ext byte is always present, kind 1), dt, the ID, one mask byte for
  each 8 payload bytes (bit i of mask j is payload byte 8j+i; the length comes from the DLC), then the changed
  bytes in order. A client rebuilds the payload from its entry and the changed bytes.
- **Cost.** About 18 KiB of PSRAM a session with 256 identifiers.

## The sender

One thread, `can_monitor_tx`, serves every session, preemptible, at `EERIE_LEAP_CAN_MONITOR_SENDER_PRIORITY`:
below the cooperative bus threads, and above a renderer it must not wait for.

- **Size.** A notification takes the link's `notify_max` (MTU - 3: 495 at an MTU of 498), at most 512 bytes. It
  holds the staged `statistics` record first, then waiting `tx event` records, then whole ring records.
- **Flush.** It sends when the ring holds a full notification, `EERIE_LEAP_CAN_MONITOR_FLUSH_MS` (20) after a
  record first waited, or when the session is ending.
- **In flight.** At most `EERIE_LEAP_CAN_MONITOR_IN_FLIGHT` (4) notifications, over all sessions, wait for
  their completion, which leaves the rest of the link's ATT buffers to SMP and `live`.
- **SMP first.** While the SMP router is notifying a response (`SmpBleRouter::IsNotifying()`) the sender sends
  nothing and looks again after 5 ms: a stream never delays an answer.
- **No buffer.** A notify the stack refuses with `-ENOMEM` is built once and tried again after the next
  completion, or after 5 ms with none of the stream's in flight (100 ms otherwise), never dropped, so `seq`
  has no gap. `-ENOTCONN`, a link going away before its disconnect is reported, waits the same way; the
  disconnect then frees the sessions. Any other refusal drops the notification: `seq` advances and the frames
  are counted lost.
- **Several sessions** are served in turn.

The ring is a single-producer byte ring in PSRAM: the receive thread (a meta-IRQ on the gauge, which cannot
take a lock or wait) writes records already encoded, and the sender reads them. A frame that does not fit is
lost and counted, never waited for.

## The link

The `bus` characteristic is notify-only and appended after `info`, so no handle moves; see
[bluetooth.md](bluetooth.md#gatt-service). A bonded central that cached the table learns of it through Service
Changed, which needs `CONFIG_BT_GATT_SERVICE_CHANGED`.

The access point asks for the short connection interval and the 2M PHY only while SMP writes arrive, and for
the long one 3 s after the last write; a stream makes no writes. A session therefore **holds** the fast
parameters from its OPEN until its `end` record is built, the drain included (`HoldFast()`, counted), and a
hold that leaves the interval above 15 ms is followed once by a request for 15-30 ms, which Apple's accessory
rules accept.

What the link carries depends on the central's interval, PHY and MTU. A fully loaded 500 kbit/s bus is about
4,500 frames a second, about 58 kB/s of 13-byte records, which the default 256 KiB ring holds for about 4.5 s
(2.2 s at 1 Mbit/s): the link can stall about that long without a loss. These are sizing figures, not
guarantees; the synthetic source measures a central.

## Statistics and bus state

A **state** record (kind 2) is written for each change of the controller's state, with TEC and REC. STATS
answers the state and counters as of the request.

A **statistics** record (kind 3) comes about once a second, first one second after OPEN (a late pass does not
owe the seconds it missed), and once more before the `end` record. Its 13 values count what happened **since
the previous statistics record**, so their sum is the total; STATS gives the totals since OPEN. In the order of
the record:

| Value | Counts |
| --- | --- |
| frames received | Frames from the bus, before the unit filter, remote frames included, and including those lost afterwards to a full ring. Not the unit's own. |
| frames sent | The unit's own frames the controller completed without error, before the unit filter. |
| `busy_us` | The bit time of those received and sent frames, at the session's bitrates, with no stuff bits (see below). 0 until the bitrate is known and on a synthetic session. |
| errors: bit, stuff, CRC, form, acknowledgement | Zephyr's controller error counts since OPEN. They are the controller's, so on a TAP session they include what the unit itself saw. |
| lost: controller, receive queue, ring, echoes | Frames lost at each stage, in this order (the gap causes 3, 2, 1, 4). The ring's count includes frames the sender discarded after the ring gave them up. |
| filtered | Frames the unit filter left out. |

- **Load** is `busy_us` over the second; the app works it out, the unit does not divide. The bit time is
  that of the ISO 11898-1 frame without stuff bits (a classical frame is 47 + 8n bits with an 11-bit ID and
  67 + 8n with a 29-bit one, the interframe space included; an FD frame with BRS is its arbitration and its end
  at the nominal bitrate and the rest at the data bitrate), so a loaded bus reads a little low.
- **The error counts** need `CONFIG_STATS` and `CONFIG_CAN_STATS` on the unit; without them the five error
  counts and the controller's overruns are 0.
- **There are no error frames to stream.** Zephyr reports state changes with the counters, and the counts
  above, and nothing per error; the client's "error frames" are these counts.
- **The unit filter** (OPEN's `filters`, replaced by FILTER) is a list of up to `filters_max` entries
  `[id, mask, ext]`. A frame passes when `(id & mask)` equals the entry's and its IDE equals `ext`; an empty
  list passes every frame. It is applied before the frame is encoded and after it was counted as received.

## Transmit and replay

Built only with `EERIE_LEAP_CAN_MONITOR_TX`. STATUS `tx` says whether the build has it; without it ARM, SEND,
PERIODIC and REPLAY answer `TX_UNSUPPORTED`, whatever the token. The requests are in
[smp.md](smp.md#canmon-can-monitor-sessions) and `src/configuration/cbor/cbor_schemas/can_monitor_smp.cddl`.
The unit sends frames with `Canbus::SendFrame(const CanFrame&)`, which keeps the frame's own IDE, FDF, BRS and
RTR (see [canbus.md](canbus.md#subsystem)). Every frame a session sends comes back to its stream as a `frame`
record with TX set (TX failed when the driver reported an error).

**Who transmits.** `TAP` and `ACTIVE` sessions. A `LISTEN_ONLY` or synthetic session has no transmitter: ARM
and the rest answer `BAD_MODE`.

**Arming.** Nothing is sent before `ARM {tok, on: true}`, which answers `{armed, share_pct, reserved}` and takes
the session's bitrates and the COM channel's reserved IDs as they are then. It answers `BAD_BITRATE` while the
bus is still detecting its bitrate and `BUS_OFF` while the bus is off; arming an armed session changes nothing.
The unit asks nothing of its own user before it transmits; ARM is an SMP write, so the link is encrypted. SEND,
PERIODIC and a started REPLAY need an armed session (`NOT_ARMED`) and a bus that is not off (`BUS_OFF`).

**Disarming** stops the periodic frames and the replay. ARM off, CLOSE, a lapsed lease, the end of the
connection, a stored CAN configuration and a state change to bus-off disarm; the bus has to leave bus-off before
ARM works again. A `tx event` record says it: 1 disarmed or 2 bus-off, with the number of frames not sent (the
frames still due of periodic slots that have a `count`, and the frames left in the replay). Skipped frames not
yet reported (below) go first. A session that ends armed puts this record before its `end` record.

**The cap.** A token bucket on bus time, as SMP over CAN is paced (see [cdmp.md](cdmp.md)): each frame costs its
bit time (without stuff bits), and the bucket refills at `EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT` (50) of the
elapsed time. It holds `EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES` (8) times the time of the largest frame the
channel carries (a 64-byte 29-bit FD frame with BRS on an FD channel, an 8-byte 29-bit frame otherwise), so
that many back-to-back frames follow an idle bus. SEND stops at the first frame over the cap; a replay frame
over it waits for the credit; a periodic frame over it is skipped.

**Reserved IDs.** When the session's port is the stored COM configuration's `bus_channel` (the port that carries
CDMP and SMP), the network's identifiers are not sent: the 11-bit `cdmp_base_can_id` to +99 and the 29-bit
`smp_can_id_base` to `| 0xFFFF` (see [canbus.md](canbus.md#stored-configuration)). ARM states the ranges
(`[from, to, ext]`; none on any other port). A SEND or PERIODIC request with one is refused `RESERVED` whole;
a replay frame with one is skipped. Nothing else on the channel is refused.

**SEND.** `frames: [[id, flags, data]]`, 1 to 16, flags being bits IDE (0), FDF (1), BRS (2) and RTR (3), `data`
a byte string of at most 64 bytes. A frame the bus cannot carry (an unknown flag, an ID that does not fit its
format, BRS without FDF, FD on a classical channel, a remote frame with FDF, more than 8 bytes without FDF, a
length no DLC gives) or a reserved ID refuses the whole request before anything is sent. A remote frame
sends no data: the length of `data` is the length it asks for (0 to 8). Then the frames go in order, each
through the cap; the first that the cap holds back or the bus refuses ends the request. The answer is
`{sent, refused}`; when none went it is `RATE` (the cap) or `UNKNOWN` (the bus).

**PERIODIC.** `list: [{slot, id, flags, data, period_ms, ? count}]` replaces the list; an empty list stops every
slot. At most `EERIE_LEAP_CAN_MONITOR_PERIODIC_SLOTS` (16) entries, with distinct slots and a period of 1 ms or
more, each frame as SEND checks it; a request that cannot be kept leaves the list as it was. Each slot sends at
once and then every `period_ms` from the domain's own work queue, at a fixed rate (a slot that fell a period
behind does not burst to catch up). The work queue runs on the kernel tick, so a period shorter than a tick
(`CONFIG_SYS_CLOCK_TICKS_PER_SEC`: 100 µs on the S3 build, 1 ms on the P4) is rounded up to whole ticks, and so is the
replay's timing. A frame the cap holds back or the bus refuses is skipped and does not count towards `count`; a slot
with a `count` stops after that many frames sent, and `count` 0 sends none.

**Skipped frames** (the periodic ones, and a replay's frames with a reserved ID or that the bus refused) are
counted and reported as `tx event` 3 with the number skipped since the last report: the first at once, then at
most once a second, so a count still waiting goes out at the end of that second (or at the disarm).

**REPLAY.** The app cannot time frames over Bluetooth, so the unit plays a buffer the app keeps filled ahead.
`{tok, op: 0, ? speed_pct, ? loop}` starts a replay (speed 100 by default, 1 to 10000; a replay already running
is replaced); `{tok, op: 1, seq, data}` adds a chunk; `{tok, op: 2}` stops; `{tok, op: 3}` says no more chunks
will come. Every answer carries `{room}`, the buffer's free bytes (all of it after the start and the stop).

- **Chunks** are the stream's own records, whole kind 0 frames and no other kind: the dt is the time between
  frames in the capture, which the replay divides by the speed. ESI is not carried. Chunks come in order of
  `seq` from 0 (`BAD_SEQ`, also for a chunk or `op: 3` with no replay running or one that ended) and are at most
  `room` bytes (`NO_ROOM`); a chunk that is not whole kind 0 records is a bad request (`EINVAL`). A refused
  chunk changes nothing and may be sent again.
- **The buffer** holds `EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE` (64 KiB) from the external memory resource,
  made at the start (`NO_MEMORY` if it cannot be); `room` is what is not waiting to be played.
- **Playback** sends each frame at its time, through the cap. The first frame is due its own dt after the start.
  A frame more than `EERIE_LEAP_CAN_MONITOR_REPLAY_LATE_MS` (10) late is sent at once and the frames behind it
  count from the time it went, so a stall does not turn into a burst; the lateness is not reported.
- **Underrun.** An empty buffer before `op: 3` is an underrun, `tx event` 5 with the number of underruns so far;
  playing goes on when a chunk comes, its first frame due its dt after the chunk arrived.
- **Loop.** After `op: 3` the replay starts over when the last frame has gone, but only while the whole replay
  still fits in the buffer: the played bytes are kept for it until a chunk needs their room, and then `loop` no
  longer applies and the app sends the range again. A replay that loops ends only by STOP or a disarm.
- **The end.** A replay that played its last frame ends with `tx event` 4, the count being the frames played
  (skipped ones included). STOP ends one silently.

**The records.** Tx events are written by the sender, not the receive thread: they go into the session's next
notification ahead of its ring records, with dt 0, so their place in the stream is not their time. While the
link is stalled a session keeps at most one waiting event of each kind: a new one of a kind already waiting joins
it (the counts add up; an underrun's count, the underruns so far, replaces the waiting one), so none is dropped
and the events keep the order their kinds first waited in.

## Limits and caveats

- **Receive times.** A frame is timed in the receive callback with the 64-bit cycle counter
  (`CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER`; 240 MHz on the ESP32-S3, 16 MHz on the P4), which resolves frames a
  few hundred µs apart. A target without it times frames by the kernel tick (100 µs to 1 ms), so frames closer
  than a tick share a time. See [canbus.md](canbus.md#receive-times).
- **The MCP2518FD's SPI lag.** On this controller the callback runs in the driver's interrupt thread after the
  frame was read over SPI, so a time lags the wire by that read and by what the thread waited for. The TWAI has
  no controller timestamp, and Zephyr's own is 16 bits of the controller's timer, so neither is used. Treat a
  time as the receive time, not the wire time.
- **The filter swap** may repeat or miss frames around the `tap` record (above). The unit's consumers see the
  same frames in the same order as without a tap.
- **Remote frames** reach the stream (RTR set) and never the unit's consumers; they need
  `CONFIG_CAN_ACCEPT_RTR`, else both drivers drop them.
- **The unit's own frames** appear as TX only while a tap is set and a slot is free, at their completion.
- **A bus detecting its bitrate** streams nothing, and `busy_us` is 0, until the bitrate is found.
- **Load.** With a tap set the controller hands over every frame on the bus, and the receive thread stamps,
  converts, dispatches and taps each one. On the MCP2518FD the driver's interrupt thread first reads each frame
  over SPI, at 8 MHz at most with a 20 MHz oscillator; `patches/zephyr/can_mcp251xfd_rx_read_by_dlc.patch` has a
  CAN FD build read a frame's header and first 8 data bytes, and the rest only for a longer payload, instead of the
  whole 72-byte item. A classical frame then costs the gauge about 80 µs in that thread and 20 µs in the receive
  thread, against 120 µs for a frame on a 1 Mbit/s bus, so a burst of back-to-back frames overruns the controller's
  FIFO whenever the thread is held up. Zephyr's interrupt thread also slept 10 ms after 10 passes of its loop that
  found the interrupt line still asserted, a guard against a flag it cannot clear that a long burst also tripped,
  overrunning the FIFO once a burst; `patches/zephyr/can_mcp251xfd_sleep_only_when_stalled.patch` keeps that sleep
  for passes that read no frame, and has a pass that did pause a tick (100 µs on the S3) every 10, which the FIFO
  bridges. Measure on the unit with its own load (its display, its sensors).
- **Memory.** Each session takes a ring (`EERIE_LEAP_CAN_MONITOR_RING_SIZE`, 256 KiB) from the external memory
  resource, allocated at OPEN (`NO_MEMORY` when it cannot be had; the size must be a power of two), a changed-bytes
  table with `delta`, and the replay buffer while a replay runs. In internal RAM: the receive queue's
  entries (about 90 bytes each, see [canbus.md](canbus.md#the-tap)), the pool of
  `EERIE_LEAP_CAN_MONITOR_TX_SLOTS` frames (about 80 bytes a slot, made at the first tap), and the sender's
  buffers (a 512-byte notification and about 650 bytes of scratch).
- **Threads.** The sender (stack `EERIE_LEAP_CAN_MONITOR_SENDER_STACK_SIZE`), the domain's work queue (leases,
  disconnects, periodic frames, replay; `EERIE_LEAP_CAN_MONITOR_WORK_QUEUE_STACK_SIZE`), a thread for a
  synthetic session, and the bus thread of a session's own bus. The sender and the work queue catch and throw
  exceptions; where unwinding uses DWARF tables (Xtensa, RISC-V, MIPS, AArch64) they need several KiB more than
  the defaults (the tests give the sender 10 KiB), and a stack that is too small overflows silently.

## Kconfig

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_CAN_MONITOR` | n | The domain. Depends on `EERIE_LEAP_DOMAIN_CANBUS`; selects `EERIE_LEAP_THREADING`. With `EERIE_LEAP_SMP` it also builds the `canmon` group and selects `MCUMGR_SMP_CBOR_MIN_DECODING_LEVEL_4` (OPEN, FILTER, SEND and PERIODIC nest lists in the request map) and, with `ZCBOR_CANONICAL`, `MCUMGR_SMP_CBOR_MIN_ENCODING_LEVEL_4` (STATUS answers a list of maps that hold a list). |
| `EERIE_LEAP_CONTROLLER_CAN_MONITOR` | n | `CanMonitorController`. Depends on `EERIE_LEAP_DOMAIN_CAN_MONITOR`, `EERIE_LEAP_SMP`, `EERIE_LEAP_CONTROLLER_CANBUS` and `EERIE_LEAP_CONTROLLER_BLE`. |
| `EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS` | 1..8, 2 | Sessions at once, at most one a port. |
| `EERIE_LEAP_CAN_MONITOR_LEASE_MS` | 100..600000, 5000 | A session that no request with its token renews for this long ends. |
| `EERIE_LEAP_CAN_MONITOR_RING_SIZE` | 1024..16777216, 262144 | Bytes of each session's ring, from the external memory resource. A power of two. |
| `EERIE_LEAP_CAN_MONITOR_FLUSH_MS` | 1..1000, 20 | Longest a record waits for its notification to fill. |
| `EERIE_LEAP_CAN_MONITOR_IN_FLIGHT` | 1..16, 4 | Notifications awaiting completion; the rest of the ATT buffers stay for SMP and `live`. |
| `EERIE_LEAP_CAN_MONITOR_SENDER_PRIORITY` | 0..127, 4 | The sender thread's priority (preemptible). |
| `EERIE_LEAP_CAN_MONITOR_SENDER_STACK_SIZE` | 2048 | Its stack. |
| `EERIE_LEAP_CAN_MONITOR_WORK_QUEUE_STACK_SIZE` | 4096 | The domain's work queue: lease lapses, disconnects (which stop an own bus, waiting for its thread) and, with `EERIE_LEAP_CAN_MONITOR_TX`, the periodic frames and the replay. It needs room to unwind exceptions. |
| `EERIE_LEAP_CAN_MONITOR_WORK_QUEUE_PRIORITY` | 0..127, 6 | Its priority (preemptible). |
| `EERIE_LEAP_CAN_MONITOR_MAX_FILTERS` | 1..64, 16 | Unit filters a session takes (STATUS `filters_max`). |
| `EERIE_LEAP_CAN_MONITOR_DELTA` | n | Changed-bytes records. |
| `EERIE_LEAP_CAN_MONITOR_DELTA_IDS` | 1..4096, 256 | Identifiers whose last payload a session keeps. |
| `EERIE_LEAP_CAN_MONITOR_TX` | n | Transmit and replay. |
| `EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT` | 1..100, 50 | The cap on the bus time a session transmits. |
| `EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES` | 1..64, 8 | The bucket's depth in largest frames. |
| `EERIE_LEAP_CAN_MONITOR_PERIODIC_SLOTS` | 1..64, 16 | Periodic frames a session sends. |
| `EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE` | 1024..16777216, 65536 | Bytes of the replay buffer, from the external memory resource, allocated while a replay runs. |
| `EERIE_LEAP_CAN_MONITOR_REPLAY_LATE_MS` | 1..10000, 10 | A replayed frame later than this is counted late. |
| `EERIE_LEAP_CAN_MONITOR_SYNTHETIC` | n | The synthetic source, for debug and bench builds. |
| `EERIE_LEAP_CAN_MONITOR_TX_SLOTS` | 1..64, 16 | Frames in flight the tap's echo can follow; in the CAN subsystem (see [canbus.md](canbus.md#subsystem)). |
| `EERIE_LEAP_CAN_MONITOR_LOG_LEVEL_*` | | Log level of the domain. |

Zephyr options a unit sets:

| Option | Why |
| --- | --- |
| `CONFIG_CAN_ACCEPT_RTR=y` | Both drivers drop remote frames otherwise. |
| `CONFIG_STATS=y`, `CONFIG_CAN_STATS=y` | The error counts and the controller's overruns; 0 without them. |
| `CONFIG_BT_GATT_SERVICE_CHANGED=y` | A bonded central learns of `bus`. |

The synthetic source (`OPEN {synthetic: {fps, ids, dlc, ? ext, ? fd}}`, 1 to 200000 frames a second, 1 to 2048
IDs, DLC 0-8 or up to 15 with `fd`) feeds a session's ring with no bus (port 255), `ids` distinct IDs in turn
(11-bit from 0, or 29-bit from `0x18DA0000`), the payload starting with the frame's number as a little-endian
counter and then `0xA5 ^ i`. The sender, the link and the client see an ordinary session; raise `fps` until
`gap` records appear to find the ceiling.

## Wiring

`CanMonitorController(CanbusController&, BleService&)` is created after `CanbusController` and `BleController`
have been initialized: it takes the CAN service from the one and the access point from the other.
`Initialize()`:

- builds `CanMonitorService` with the devicetree's CAN ports (`DtCanbus::Get`) and a link made of the `bus`
  notifier (`BleService::GetBusNotifier()`), the fast-link hold (`HoldFast()`, `ReleaseFast()`) and the
  SMP-first check (`IsSmpNotifying()`);
- routes the `bus` completions (`BleService::SetBusSentHandler()`) and Ble's disconnected handler to it, which
  ends every session without a record;
- registers the service with `CanbusController::RegisterDependentService()`, so that a stored CAN configuration
  stops it, ending every session with the reason `configuration`, before `CanbusService::Configure()` replaces
  the buses, and starts it after (OPEN answers `BUSY` in between). The new configuration may take a port a
  session held, so the client offers to reopen;
- registers the `canmon` group.

`Start()` lets OPEN in. A firmware update needs nothing special: the client closes its session while it
updates, and the unit restarts to install the image.

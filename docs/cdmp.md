# CDMP and the CAN COM channel

CDMP, the CAN Device Management Protocol, lets EerieLeap units on one CAN bus discover each
other, claim device IDs, exchange heartbeats and commands, and carry SMP (MCUmgr) packets. The
protocol itself is specified in [`src/subsys/cdmp/README.md`](../src/subsys/cdmp/README.md);
this page covers the implementation and its configuration.

Code: `src/subsys/cdmp/` (protocol), `src/subsys/smp/can/` (SMP over CAN),
`src/domain/canbus_com_domain/` (the COM service that runs both on the configured channel).

## Identity

| Item | Source |
| --- | --- |
| Device type | `EERIE_LEAP_DOMAIN_CANBUS_COM_DEVICE_TYPE_*` choice: `NONE` (0), `LOGGER` (1), `DISPLAY` (2). |
| UID (32-bit) | `CdmpUid::Generate()`: CRC32 of the SoC hardware ID (eFuse MAC on ESP32), so it is stable across reboots; random when the SoC reports none. |
| Device ID (1..254) | Claimed at start by discovery, lowest free ID first; 0 is unassigned, 255 broadcast. |
| Protocol version | 1.0 (`0x10`). A unit with another major version puts the whole network into `VERSION_MISMATCH`. |

## Services

`CdmpService` is one CDMP node. It owns a thread (`EERIE_LEAP_CDMP_SERVICE_THREAD_*`) that
starts the sub-services and a work queue (`EERIE_LEAP_CDMP_WORK_QUEUE_*`) they and the SMP
transport run on:

- **Network service**: discovery requests and responses, ID claim and conflict handling, the
  table of other devices (updated from heartbeats, validated every 5 s, a device is dropped after
  3 missed heartbeats). Exposed read-only as `ICdmpNetworkInfo` for the SMP `device` and
  `network` groups.
  - Every unit records an ID claim as pending and adds the claimant on its first heartbeat, so a
    unit that does not hold the lowest ID also learns later joiners. A pending claim expires after
    the heartbeat timeout. Answering claims stays with the lowest ID and the claimed ID's owner.
  - An online unit that hears a heartbeat from a device it does not know, such as one removed
    while its cable was unplugged, sends a discovery request, at most once per heartbeat interval,
    and adds the device again from its response.
  - Discovery and the ID claim send one request per step of a work queue task, so the responses
    are processed while they wait: a discovery response ends discovery, a reject moves the claim to
    the next free ID, and a version-incompatible answer ends in `VERSION_MISMATCH`. The claim skips
    IDs that are in the table or have a pending claim.
  - A stop drops the frames still queued for the service, and a start begins with an empty table.
  - `RegisterDeviceEventHandler()` reports devices that join, restart (a claim of their ID with
    the same UID) or leave the table, and `RegisterHeartbeatHandler()` passes on every heartbeat
    of a device in the table. Both run on the CDMP work queue and must not block.
- **Heartbeat service**: sends a heartbeat every 3 s while online. Every heartbeat is 8 bytes,
  with byte 7 carrying a digest of the unit's state records (0 without records), and a shorter
  frame is dropped as malformed.
- **Command service**: request/response with transaction IDs, retries and a per-transaction
  timeout (`EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS`). Service commands `STATUS_REQUEST`
  (0x10), `RESET_DEVICE` (0x11) and `STATE_SYNC_REQUEST` (0x12) are handled by the protocol;
  application commands use codes 0x20..0xFF. Responses are broadcast and every unit numbers its
  own transactions, so a response completes a transaction only if its command code matches too
  and, for a request to one device, it comes from that device.
- **State service** (`CdmpStateService`, `GetStateService()`): shared state records on Base + 4/5
  (section 4.3 of the spec), described under [Shared state](#shared-state). It lives as long as
  the node, so this unit's records survive a stop.

`CdmpService::Configure(canbus, base_can_id)` binds the services to a bus; the management IDs
are `base + 0..5`, capability streams `base + 20..51`, application IDs `base + 52..99`.

With a base that is a multiple of 8 (the default 0x700 is), the services share one CAN filter on
`base + 0..7`, so CDMP takes one controller filter and SMP over CAN one more; the rest are left to
the sensors' CAN routes. Any other base takes one filter per management ID, six in all. A route
inside `base + 0..7` overlaps the shared filter: it is refused if CDMP registered first, and
otherwise CDMP falls back to the six exact filters.

## COM service

`CanbusComService` (`EERIE_LEAP_DOMAIN_CANBUS_COM`, selects `EERIE_LEAP_SMP_CAN`) runs CDMP
and the SMP-CAN transport on the channel named by the stored CAN configuration's `com_config`
(see [canbus.md](canbus.md)). It is created by the CAN controller and:

- stops when `CanbusService` resets its buses and starts again once they are configured, so a CAN
  reconfiguration restarts the whole node;
- configures the transport with `smp_can_id_base` and `smp_bus_share_percent`, and CDMP with
  `cdmp_base_can_id`;
- binds the SMP transport to the claimed device ID when the node is `ONLINE` or
  `VERSION_MISMATCH` (so a unit with another CDMP version can still be managed) and unbinds it
  otherwise;
- implements `ISmpForwarder`, which the BLE access point uses to reach other units;
- implements `INetworkStateTransport`, which shares state records with the other units (see
  [Shared state](#shared-state)).

**Application commands.** Register a handler for a command code with a request type that is
constructible from a byte span and returns success, or a result carrying up to 4 bytes:

```cpp
canbus_com_service->SetCommandHandler<CanbusComLoggingCommand>(
    CanbusComCommandCode::LOGGING,                      // 0x20
    [this](std::optional<CanbusComLoggingCommand> command) {
        return command.has_value() && command->IsStart() ? LogWriterStart() : LogWriterStop();
    });
```

`SendCommand(command, callback, device_id)` sends to one device or, by default, broadcasts
(255); the callback receives the result code and up to 4 bytes of response data. rt_core defines
one command, `LOGGING` (0x20, one byte: 0x01 start, 0x00 stop), which rt_daq uses to start and
stop logging on every unit at once.

## Shared state

A unit shares a few bytes of its state, such as whether it is logging, with every other unit on
the network (section 4.3 of the spec). CDMP delivers the bytes and keeps the copies consistent, but
never interprets them.

**Records.**

- A unit holds at most one record per state type: the type and 1 to 5 data bytes. The other units
  keep only the latest value of each record, so a late joiner, a restart and a lost frame all
  converge on the owner's current state. A record is state, not an event.
- Every change of a unit's records, a new value or a withdrawal, increments the unit's 8-bit
  version counter and goes out as a notification on Base + 4. Every other online unit acknowledges
  it on Base + 5, and the change is resent after 50, 100 and 200 ms until the first 8 of them have;
  the digest below covers the rest. A record that other changes leave 64 versions behind is sent
  again under a new version, so that receivers never take its next change for an old one.
- Byte 7 of the heartbeat is a CRC-8 digest of the sender's records. A receiver whose copies
  differ marks them stale and asks the sender to replay its records with `STATE_SYNC_REQUEST`
  (0x12), at most once per heartbeat, so a divergence lasts about one heartbeat interval.
- A unit that enters `ONLINE` sends all its records and broadcasts `STATE_SYNC_REQUEST`.
- Copies follow the device table. When a unit claims its ID again (it restarted), its copies turn
  stale: they keep their value, and its next record of each type replaces them whatever its
  version. A unit that leaves the table takes its copies along, and a unit that leaves `ONLINE`
  drops every copy but keeps its own records. Notifications from a unit that is not in the table
  yet are not acknowledged, so the sender resends them once its first heartbeat has added it.
- A unit keeps up to `EERIE_LEAP_CDMP_STATE_MAX_LOCAL_RECORDS` records of its own and
  `EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS` copies, in fixed arrays. Copies that do not fit are
  dropped with one warning, and their source's digest is not checked until room frees up.

**State types.** `CanbusComStateType` (`states/canbus_com_state_type.h` in the COM domain) is the
registry of the types units share, the counterpart of `CanbusComCommandCode`, in the application
range 0x10..0xFF:

| Type | Code | Data |
| --- | --- | --- |
| `LOGGING_STATUS` | 0x10 | Whether a unit logs, and why it stopped: flags (bit 0 active), then the `LoggingStatusReason` while inactive (`CanbusComLoggingState`, see [logging.md](logging.md#status-on-other-units)). |

Codes and data layouts are part of the network contract: add types and append fields, but never
renumber or reuse them. A state type implements `ICanbusComState` (`GetStateType()`, `GetData()`)
and is constructible from a record's data bytes, which the `CanbusComState` concept checks: the
counterpart of `ICanbusComCommand`. Its decoder ignores trailing bytes it does not know and gives
missing trailing fields their defaults. `CanbusComStateData` holds the data bytes without
allocation, and `RemoteStateRecord` is another unit's record, `{device_id, data}`.

**`INetworkStateTransport`.** `CanbusComService` shares records through this interface
(`services/i_network_state_transport.h` in the COM domain), so that code built on shared state can
be tested with a fake:

| Method | Effect |
| --- | --- |
| `SetLocalState(type, data)`, `SetLocalState(state)` | Shares this unit's record of the type. Setting the current value again sends nothing, and empty data withdraws the record. False when every record slot holds another type. |
| `ClearLocalState(type)` | Withdraws this unit's record of the type. |
| `RegisterStateHandler(type, handler)` | Reports that other units' records of the type changed: once right after registration, so that a late subscriber catches up, then whenever one is added, changed or removed, which includes every copy being dropped when this unit leaves `ONLINE`. Returns an ID. |
| `UnregisterStateHandler(id)` | Once it returns, the handler is not running and is not called again. |
| `GetRemoteStates(type, records)` | Copies the other units' records of the type, ordered by device ID, into a caller-owned span, and returns the number copied. A span of `INetworkStateTransport::MAX_REMOTE_RECORDS` (`EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS`) takes every unit's record; a shorter one takes those of the lowest IDs and gives no sign of the rest. |

```cpp
// A unit that logs shares its status...
canbus_com_service_->SetLocalState(CanbusComStateType::LOGGING_STATUS, CanbusComStateData(status_bytes));

// ...and a unit that shows it reads every unit's record whenever one changes.
handler_id_ = canbus_com_service_->RegisterStateHandler(
    CanbusComStateType::LOGGING_STATUS,
    [this](CanbusComStateType state_type) {
        // Room for every unit's record, so that the combined state misses none.
        std::array<RemoteStateRecord, INetworkStateTransport::MAX_REMOTE_RECORDS> records{};
        const size_t count = canbus_com_service_->GetRemoteStates(state_type, records);
        // Combine std::span(records).first(count) and publish the result with PublishAsync().
    });
```

The records live in the state service, which lives as long as the COM service. Calls made while
the node is stopped, or before `Initialize()`, update this unit's records, which are announced on
the next `ONLINE`, and the records and the handlers survive the stop and start of every CAN
reconfiguration.

**Threading.** No thread is added:

- `SetLocalState()`, `ClearLocalState()` and `GetRemoteStates()` may be called from any thread.
  Sending is handed over to the CDMP work queue.
- Frames, resends and handlers run on the CDMP work queue, and receiving allocates nothing. A
  handler must not block, so it reads the records and publishes with `PublishAsync()`; it must not
  register or unregister handlers. A buffer of `MAX_REMOTE_RECORDS` records takes 7 bytes each on
  the work queue's stack (224 bytes by default); with a much larger capacity, keep it as a member
  that only the handler uses.
- `GetRemoteStates()` copies the records under the state service's lock, so the copy is one
  instant's records, and a handler that reads it sees at least the change it is called for.

**State bridge.** `state_bridge/` in the COM domain moves a record between an event channel and the
network, so that a domain shares its state without knowing CDMP, and its consumers see other units'
state as ordinary events. A descriptor in the domain that owns the state (the
`NetworkStateDescriptor` concept, `state_bridge/network_state_descriptor.h`) names the channel, the
event type, the state type and its record type `State` (a `CanbusComState` that is also default
constructible and equality comparable), and maps between them:

| Member | Returns |
| --- | --- |
| `Initial()` | A unit's record before its first event. |
| `FromEvent(event, current)` | A unit's record after the event, given its current one, or nothing when the event does not change it. |
| `Combine(states)` | The state consumers see, given every other unit's record ordered by device ID; an empty span when no unit reports one. |
| `ToEvent(state)` | The event that carries a combined state. |

- `NetworkStateExporter<Descriptor>`, on a unit that produces the state, shares `Initial()` on
  construction, then the record each event maps to when it changes, and withdraws the record on
  destruction. It handles events on the channel's bus thread. A record the transport refuses
  (every record slot holds another state type) is offered again with each later event.
- `NetworkStateImporter<Descriptor>`, on a unit that consumes the state, decodes and combines every
  other unit's record from its state handler, once right after construction and then on each
  change, and publishes `ToEvent()` of the result with `PublishAsync()` when it differs from the
  state it published last. While no bus carries the channel it publishes nothing and counts
  nothing as published, so its first call after a bus registers the channel publishes.
- Imported events carry `IMPORTED_EVENT_SOURCE_ID`, and exporters skip events with it, so a unit
  never shares back what it imported. A unit either exports or imports a descriptor, never both.

Both are header-only, take the transport as `std::shared_ptr<INetworkStateTransport>`, and throw
`std::runtime_error` when they cannot subscribe or register. A bus has to carry the channel before
they are built. `LoggingNetworkState` is the first
descriptor ([logging.md](logging.md#status-on-other-units)). Another shared state is a
`CanbusComStateType`, a record type, a descriptor, and one exporter or importer per application.

## SMP over CAN

`SmpCanTransport` is an MCUmgr transport (section 5 of the CDMP README):

- Identifier `smp_can_id_base | target << 8 | source`, 29-bit; the unit accepts frames whose
  target is its own device ID from any source.
- Frames carry one control byte (`start` flag and a 7-bit sequence) and 7 data bytes on classical
  CAN or 63 on CAN FD, following the bus type. A short last CAN FD frame is padded to a valid
  length.
- Reception keeps `EERIE_LEAP_SMP_CAN_RX_SLOTS` reassembly slots, one per source with a packet in
  progress; a packet is dropped on a sequence gap, an oversize, a malformed frame, or
  `EERIE_LEAP_SMP_CAN_RX_TIMEOUT_MS` without a frame. There is no negative acknowledgement; the
  client resends after its own timeout.
- Sending is paced with a token bucket to `smp_bus_share_percent` of the bus time, assuming
  worst-case frame lengths (160 bit times on classical CAN; 60 nominal plus 700 data bit times on
  CAN FD), with bursts of up to `EERIE_LEAP_SMP_CAN_TX_BURST_FRAMES` frames after idle. One
  packet at a time per target. At the default 25 %, roughly 5.5 KB/s at 500 kbit/s classical and
  33 KB/s on CAN FD with a 2 Mbit/s data phase.
- Requests addressed to this unit go to the local SMP server; responses to forwarded requests go
  to the registered response sink (the BLE router).

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_CDMP` | n | Builds the protocol. Selects `CRC`, `HWINFO`, `EERIE_LEAP_RANDOM`. |
| `EERIE_LEAP_CDMP_SERVICE_THREAD_PRIORITY` | 5 | CDMP service thread. |
| `EERIE_LEAP_CDMP_SERVICE_THREAD_STACK_SIZE` | 4096 | |
| `EERIE_LEAP_CDMP_WORK_QUEUE_PRIORITY` | 5 | Work queue of the sub-services and the SMP-CAN sender. |
| `EERIE_LEAP_CDMP_WORK_QUEUE_STACK_SIZE` | 4096 | |
| `EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS` | 500 | Time to wait for a command response. |
| `EERIE_LEAP_CDMP_STATE_MAX_LOCAL_RECORDS` | 1..32, 8 | State types this unit can share at once. |
| `EERIE_LEAP_CDMP_STATE_MAX_REMOTE_RECORDS` | 1..1024, 32 | Records of other units kept, about 10 bytes each. Also sizes a span that takes every unit's record of one type (`INetworkStateTransport::MAX_REMOTE_RECORDS`). |
| `EERIE_LEAP_DOMAIN_CANBUS_COM` | n | Builds the COM service; selects `EERIE_LEAP_SMP_CAN`. |
| `EERIE_LEAP_DOMAIN_CANBUS_COM_DEVICE_TYPE_*` | `NONE` | CDMP device type of this unit. |
| `EERIE_LEAP_SMP_CAN` | n | SMP transport over CAN. Depends on `EERIE_LEAP_CANBUS`; selects `EERIE_LEAP_SMP`. |
| `EERIE_LEAP_SMP_CAN_RX_SLOTS` | 1..8, 2 | Sources that may send a packet at the same time. |
| `EERIE_LEAP_SMP_CAN_RX_TIMEOUT_MS` | 100 | Inter-frame timeout of a partial packet. |
| `EERIE_LEAP_SMP_CAN_TX_BURST_FRAMES` | 1..64, 8 | Frames sent back to back after an idle period; should cover a few system ticks at the paced rate. |

Everything else (heartbeat interval, discovery backoffs, ID claim timeout, stagger offsets, state
resend backoffs) is a protocol constant in `src/subsys/cdmp/utilities/constants.h`.

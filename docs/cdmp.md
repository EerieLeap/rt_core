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
  (0x10) and `RESET_DEVICE` (0x11) are handled by the protocol; application commands use codes
  0x20..0xFF.
- State change notifications (Base + 4/5) are specified but not implemented.

`CdmpService::Configure(canbus, base_can_id)` binds the services to a bus; the management IDs
are `base + 0..5`, capability streams `base + 20..51`, application IDs `base + 52..99`.

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
- implements `ISmpForwarder`, which the BLE access point uses to reach other units.

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
| `EERIE_LEAP_DOMAIN_CANBUS_COM` | n | Builds the COM service; selects `EERIE_LEAP_SMP_CAN`. |
| `EERIE_LEAP_DOMAIN_CANBUS_COM_DEVICE_TYPE_*` | `NONE` | CDMP device type of this unit. |
| `EERIE_LEAP_SMP_CAN` | n | SMP transport over CAN. Depends on `EERIE_LEAP_CANBUS`; selects `EERIE_LEAP_SMP`. |
| `EERIE_LEAP_SMP_CAN_RX_SLOTS` | 1..8, 2 | Sources that may send a packet at the same time. |
| `EERIE_LEAP_SMP_CAN_RX_TIMEOUT_MS` | 100 | Inter-frame timeout of a partial packet. |
| `EERIE_LEAP_SMP_CAN_TX_BURST_FRAMES` | 1..64, 8 | Frames sent back to back after an idle period; should cover a few system ticks at the paced rate. |

Everything else (heartbeat interval, discovery backoffs, ID claim timeout, stagger offsets) is a
protocol constant in `src/subsys/cdmp/utilities/constants.h`.

# CAN bus

The **CAN subsystem** (`src/subsys/canbus/`) drives one Zephyr CAN controller per bus. The
**CAN domain** (`src/domain/canbus_domain/`) holds the stored configuration (buses, messages,
signals, the management channel), keeps the buses configured and sends the periodic messages.
CAN-fed sensors are in [sensors.md](sensors.md); the management protocol and SMP transport that
run on the COM channel are in [cdmp.md](cdmp.md).

## Subsystem

`Canbus` wraps one controller (`DtCanbus::Get(bus_channel)`, aliases `can0` and `can1`):

- **Configuration.** `CanbusConfig{device, type, bitrate, data_bitrate, extra_modes}`. Type
  `CLASSICAL_CAN` or `CANFD` (FD mode is enabled when the controller supports it). A data
  bitrate of 0 on CAN FD means the nominal bitrate.
- **Bitrate 0 is autodetect.** The bus starts in listen-only mode and tries the supported
  bitrates, most common first, until one receives at least 3 frames within a 500 ms window; it
  retries every `EERIE_LEAP_CANBUS_AUTO_DETECT_INTERVAL_MS`. On success the bus switches to
  normal mode, installs the filters registered meanwhile, and the detected bitrate is written back
  into the stored configuration by `CanbusService`. That store is a configuration update: after the
  unit has started it stops logging on every unit (see
  [logging.md](logging.md#configuration-updates)). `CanbusService` hands it to the configuration
  work queue when it has one, since the bus's thread is cooperative and has a 2 KiB stack.
- Supported bitrates: 10k, 20k, 50k, 83333, 100k, 125k, 250k, 500k, 1M bit/s; CAN FD also 2M,
  4M, 5M, 8M.
- **Receive.** The driver callback only queues the raw frame (`EERIE_LEAP_CANBUS_RX_QUEUE_SIZE`
  deep; a full queue drops and counts, `GetRxDroppedCount()`). A cooperative bottom-half thread at
  the highest priority decodes frames and calls the handlers. Give it a MetaIRQ level with
  `CONFIG_NUM_METAIRQ_PRIORITIES=1`.
- **Handlers.** `RegisterFrameReceivedHandler(filter, handler)` installs a hardware filter and
  calls the handler from the RX thread for matching frames. Exact filters are limited by the
  driver; at most 4 mask filters and 8 handlers per filter; a filter overlapping another is
  rejected (`ERR_FILTER_OVERLAP`). Identical filters share one hardware filter.
- **Send.** `SendFrame(id, data, timeout)` with a 2 ms default wait for TX queue space; returns
  `-ENETDOWN` while the bus is not running.
- **Send a frame as it is.** `SendFrame(const CanFrame&, timeout)` keeps the frame's own IDE, FDF, BRS and
  RTR (a remote frame's DLC is `frame.dlc`; the ESI is the controller's) where `SendFrame(id, data, timeout)`
  makes every frame on an FD channel an FD frame with BRS. It answers `-EINVAL` for an FD frame on a classical
  channel, BRS without FDF, a remote frame with FDF, a payload or a DLC above 8, more than 8 bytes without
  FDF, a length no DLC gives, or an ID that does not fit its format, and logs those at debug level only. The
  tap's echo (below) covers both overloads.
- `CanbusProxy` hands the active instance to consumers that outlive a reconfiguration and
  notifies them when it is replaced.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_CANBUS` | n | Builds the subsystem. Depends on `CONFIG_CAN`; CAN FD needs `CONFIG_CAN_FD_MODE`. |
| `EERIE_LEAP_CANBUS_AUTO_DETECT_INTERVAL_MS` | 5000 | Pause between autodetect attempts. |
| `EERIE_LEAP_CANBUS_RX_QUEUE_SIZE` | 1..1024, 32 | Frames buffered between the driver callback and the RX thread. |
| `EERIE_LEAP_CAN_MONITOR_TX_SLOTS` | 1..64, 16 | Frames in flight the tap's echo of the unit's own frames can follow (see [the tap](#the-tap)). Allocated at the first `SetTap()`. |
| `EERIE_LEAP_CANBUS_LOG_LEVEL_*` | | Log level of the subsystem. |

### Receive times

Every entry the driver's context queues carries `NowUs()` (`canbus_time.h`, also `Canbus::NowUs()`):
microseconds since power-on, the base of `TIME`'s `uptime_ms`. `CanFrame::time_us` is the frame's
time, and the CAN monitor stamps its own events with the same function.

- **Base.** With `CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER` it is `k_cyc_to_us_floor64(k_cycle_get_64())`,
  which resolves frames a few hundred µs apart (240 MHz on the ESP32-S3, 16 MHz on the P4). Without it
  (the Cortex-A9 and MIPS QEMU targets) it is `k_ticks_to_us_floor64(k_uptime_ticks())`, as coarse as
  the kernel tick: 100 µs on the S3 build, 1 ms on the P4, so frames closer than a tick share a time.
- **Where it is taken.** In the receive callback, in the driver's context. On the MCP2518FD that is the
  driver's interrupt thread after the frame was read over SPI, so a time lags the wire by that read and
  by whatever the thread waited for. The TWAI has no controller timestamp at all, and Zephyr's own
  `timestamp` is 16 bits of the controller's timer, so neither is used.
- **Other fields.** `CanFrame` also carries `dlc` as received (a remote frame has a length but no
  payload; a frame built for sending may leave it 0) and `is_error_state_indicator` (CAN FD ESI).

### The tap

`SetTap(ICanFrameTap*)` (`i_can_frame_tap.h`) gives one observer, a monitor, every frame on the bus; the CAN
monitor ([can_monitor.md](can_monitor.md)) is its user.
One tap a bus; `nullptr` removes it and never fails. `SetTap(tap)` returns false when the bus is not
initialized, another tap is set, or the driver rejects the filters (nothing has changed then). The tap
does not survive `DoStop()`, `Configure()` or `DoInitialize()`, and the destructor removes it, so the
driver never calls back into a stopped bus.

- **Calls.** The receive thread calls the tap under the bus's lock, as it calls handlers, so
  `SetTap(nullptr)` waits for a call in progress and none follows once it returns. All calls come from
  that one thread (cooperative, at a very high priority: a tap must not wait on anything a lower thread
  holds), so a tap that writes a single-producer ring needs no lock. `OnFrame(frame, time_us,
  is_transmit, tx_error)` comes before the frame is dispatched; `OnState(state, counters, time_us)`
  and `OnFilterSwap(may_have_missed, time_us)` complete the interface.
- **The filter swap.** Two drivers call every matching filter's callback for each frame (Zephyr's
  SJA1000 core, used by the ESP32's TWAI, and the loopback driver), so an accept-all filter beside the
  handlers' own would queue a frame twice. `SetTap(tap)` therefore swaps: it adds two accept-all
  filters (mask 0, one standard and one `CAN_FILTER_IDE`) with the receive callback, then uninstalls
  the handlers' filters. `DispatchFrame()` already matches in software, so handlers get the same
  frames in the same order. `SetTap(nullptr)` installs the handlers' filters again and removes the
  accept-all ones. A handler registered while the tap is set is recorded and installs no hardware
  filter; it gets its filter when the tap goes.
- **What the swap may cost.** For the few filter writes both sets are installed, so a frame a handler
  matches may be queued twice on those two drivers: *repeated*. Where the driver has no room for two
  more filters, the swap removes first and adds after, so a frame in between may be *missed*. Either way
  the receive thread calls `OnFilterSwap(may_have_missed, time)` once, before the first entry it
  handles after the swap; `SetTap()` only records the outcome in an atomic, which keeps the tap
  single-producer. The MCP2518FD routes a frame through the first matching filter, so it cannot repeat one.
- **Bitrate detection.** While a bus detects its bitrate (bitrate 0), `SetTap()` only records the tap.
  When detection succeeds, the accept-all filters go in instead of the handlers' ones and the receive
  thread calls `OnState()` with the controller's state, so a session can report the detected bitrate.
  The detection's own accept-all filter feeds only its counter, never the tap.
- **Remote frames.** With `CONFIG_CAN_ACCEPT_RTR` the drivers pass remote frames. The receive thread
  gives them to the tap and never dispatches them, tap or not: handlers have never been given a frame
  without a payload.
- **The unit's own frames.** The TX completion callback gets no frame, so while a tap is set `SendFrame()`
  keeps each frame in a slot of a pool of `EERIE_LEAP_CAN_MONITOR_TX_SLOTS` (a lock-free bitmap) and
  passes it as `can_send()`'s `user_data`. The completion, in the driver's context, queues a TX echo
  entry (the frame, `NowUs()`, the error) and frees the slot; the tap then hears `OnFrame(..., true,
  error)`. The loopback driver also loops each frame back as a received one; a real bus does not. With no
  free slot the frame goes out as usual and `GetTxEchoLostCount()` counts it (an echo dropped by a full
  queue counts too). The pool is allocated at the first `SetTap()` and freed with the bus; `DoStop()`
  frees every slot, because `can_stop()` aborts the frames in flight and a driver may drop them without
  a completion.
- **State changes.** `BusStateChangedCallback` (driver context) logs and queues a state entry through
  `OnBusStateChanged(state, counters)`; the receive thread calls `OnState()` with it. A full queue drops
  the entry. The loopback driver never changes state, so tests call `OnBusStateChanged()` themselves.
- **The queue entry** (`canbus_rx_entry.h`) is a trivially copyable `CanbusRxEntry`: the raw
  `can_frame`, `time_us`, a kind (`RECEIVED`, `TX_ECHO`, `STATE_CHANGE`), the TX error (int8, clamped)
  and, for a state entry, the `can_state` and error counters. About 90 bytes, so the default 32-entry
  queue is about 2.8 KiB. With no tap, the only change on the receive path is the time each entry
  carries; the receive thread ignores echo and state entries.

## Stored configuration

Schema (`canbus_config.cddl`), file `config/canbus_config.cbor`, type 3 in the `config` group:

```
CborCanSignalConfig  = [ start_bit: uint, size_bits: uint, factor: float32, offset: float32,
                         byte_order: uint, is_signed: bool, name: tstr, unit: tstr ]
CborCanMessageConfig = [ frame_id: uint, is_enabled: bool, send_interval_ms: int, script_path: tstr,
                         name: tstr, message_size: uint, signal_configs: [*CborCanSignalConfig] ]
CborCanChannelConfig = [ type: uint, is_extended_id: bool, bus_channel: uint, bitrate: uint,
                         data_bitrate: uint, message_configs: [*CborCanMessageConfig] ]
CborCanbusComConfig  = [ bus_channel: int, cdmp_base_can_id: uint, smp_can_id_base: uint, smp_bus_share_percent: uint ]
CborCanbusConfig     = [ channel_configs: [0*8 CborCanChannelConfig], com_config: CborCanbusComConfig ]
```

`is_enabled` is required: a file stored before it existed does not decode, so it is kept and the
unit runs on the default (see [configuration.md](configuration.md#configuration-managers)).

**Channel** (one per bus, keyed by `bus_channel`, 0..255, matching a `can<N>` alias):

| Field | Rules |
| --- | --- |
| `type` | 1 `CLASSICAL_CAN`, 2 `CANFD`. |
| `is_extended_id` | 29-bit identifiers for every message of the channel; frame IDs must fit the chosen width. |
| `bitrate` | A supported bitrate, or 0 to autodetect. |
| `data_bitrate` | CAN FD only, 0 or a supported bitrate ≥ `bitrate`. Must be 0 on classical CAN. |

**Message** (defines a frame the unit sends, receives, or both):

| Field | Rules |
| --- | --- |
| `frame_id` | Unique within the channel. |
| `is_enabled` | false keeps the message stored but switched off: it is not sent, its script is not loaded, and its signals feed no sensor. A client's sensor configuration that binds an enabled signal sensor to it is refused (see [sensors.md](sensors.md)). Validated like an enabled message, so switching it on cannot fail. |
| `send_interval_ms` | > 0: the unit sends the frame at this period. ≤ 0: receive-only. |
| `name` | Identifier (letters, digits, `_`), 1 to 32 characters. |
| `message_size` | Bytes, 1..8 on classical CAN, 1..64 on CAN FD. |
| `script_path` | Optional Lua script with `process_can_frame`; must exist while an SD card is mounted (see [scripting.md](scripting.md)). |

**Signal** (DBC-style layout inside the message):

| Field | Rules |
| --- | --- |
| `start_bit`, `size_bits` | DBC bit numbering; the signal must fit the message (`size_bits` ≥ 1). |
| `byte_order` | 0 `LITTLE_ENDIAN_INTEL`, 1 `BIG_ENDIAN_MOTOROLA`. |
| `is_signed` | Two's complement raw value. |
| `factor`, `offset` | `value = raw × factor + offset`; `factor` ≠ 0. |
| `name` | Identifier, unique within the message, ≤ 32 characters. When sending, the signal carries the latest value of the sensor with this ID. |
| `unit` | ≤ 32 characters. |

**COM** (the channel that carries CDMP and SMP):

| Field | Default | Rules |
| --- | --- | --- |
| `bus_channel` | -1 (none) | Must be a configured channel; -1 disables CDMP and SMP over CAN. |
| `cdmp_base_can_id` | 0x700 | 11-bit; base + 99 must fit, so at most 0x79C. |
| `smp_can_id_base` | 0x1FF00000 | 29-bit with the low 16 bits clear. |
| `smp_bus_share_percent` | 25 | 1..100, cap on the bus time this unit's SMP frames may use. |

The default configuration has no channels and no COM channel: nothing runs until one is stored.

Example through the API (trimmed from the application's test setup):

```cpp
auto configuration = make_shared_pmr<CanbusConfiguration>(Mrm::GetExtPmr());
configuration->com_configuration.bus_channel = 0;

CanChannelConfiguration channel_0(std::allocator_arg, Mrm::GetExtPmr());
channel_0.type = CanbusType::CLASSICAL_CAN;
channel_0.bus_channel = 0;
channel_0.bitrate = 1'000'000;

auto message = make_shared_pmr<CanMessageConfiguration>(Mrm::GetExtPmr());
message->frame_id = 100;
message->is_enabled = true;          // the default
message->name = "EL_FRAME_0";
message->message_size = 8;
message->send_interval_ms = 20;

CanSignalConfiguration signal(std::allocator_arg, Mrm::GetExtPmr());
signal.start_bit = 0;
signal.size_bits = 16;
signal.SetName("sensor_0");          // sent with sensor_0's latest value
signal.unit = "km/h";
message->signal_configurations.emplace_back(std::move(signal));
channel_0.message_configurations.emplace_back(std::move(message));

configuration->channel_configurations.emplace(0, std::move(channel_0));
canbus_configuration_manager->Update(*configuration);
```

## Services

**`CanbusService`** owns one `Canbus` per configured channel behind a `CanbusProxy`.
`Configure()` applies the stored configuration: it stops the existing buses, creates a bus for
every configured channel whose device exists (a missing `can<N>` alias is logged and skipped),
reconfigures and restarts the ones that already existed, and discards buses that are no longer
configured. Reset and updated handlers run before and after, which is how the COM service and
the CAN sensors follow a reconfiguration. It also answers `GetMessageConfiguration(bus, frame)`
for the sensor domain's signal lookup and `GetConfiguration()` for the check of a client's CAN
sensors (see [sensors.md](sensors.md)), and persists a detected bitrate.

**`CanbusSchedulerService`** sends the messages. For every enabled message with `send_interval_ms`
it creates one periodic task on its own work queue (stack 8 KiB, priority 6). Each run:

1. builds the payload: `message_size` zero bytes, then every signal encoded with the latest
   value of the sensor named by the signal (0 when there is no such sensor or value);
2. hands the bytes to the message's `process_can_frame` script when there is one; a non-empty
   result replaces the payload;
3. sends the frame with the channel's identifier width.

A frame with no signals and no script is sent as zeros, which is useful for scripts that compose
the whole payload. The service is pausable and restarted by the CAN controller after a
reconfiguration.

**Receiving** is done by the sensor domain: `CANBUS_ANALOG` and `CANBUS_INDICATOR` sensors
decode a signal of a configured, enabled message, `CANBUS_RAW` sensors take whole frames of any
ID whether or not a message defines it. The unit never processes a received frame for which no
sensor is configured.

## Reconfiguration

`CanbusController` (`EERIE_LEAP_CONTROLLER_CANBUS`; depends on `EERIE_LEAP_DOMAIN_CANBUS`,
`EERIE_LEAP_DOMAIN_CANBUS_COM`, `EERIE_LEAP_DEVICE_TREE`, `EERIE_LEAP_FS`) creates the
configuration manager, the `CanbusService`, the `CanbusComService` and the SMP `network`
group. Given an SD service and the asset registry it also serves `scripts/can` as the `can_scripts`
asset kind and refuses to delete a script a message's `script_path` names (see
[filesystem.md](filesystem.md#assets-eerie_leap_assets)). When a configuration is stored it waits `EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS`
(default 250 ms, restarted by every further update) so that the SMP response to the request that
stored it, which may travel over the very bus being restarted, gets out first. Then, on the
configuration work queue, it stops the COM service and every service registered with
`RegisterDependentService()` (the sensor processing service and the CAN scheduler in rt_daq),
calls `CanbusService::Configure()`, and starts them again.

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_CANBUS` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION` and `EERIE_LEAP_CANBUS`. The sensor domain selects it. |
| `EERIE_LEAP_CAN_SCRIPT_MAX_SIZE` | 1..1048576, 65536 | Largest message script an upload of the `can_scripts` kind may bring, in bytes (see [scripting.md](scripting.md)). |
| `EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS` | 250 | Delay before a stored configuration is applied to the buses. |

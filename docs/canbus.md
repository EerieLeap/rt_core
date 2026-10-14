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
  into the stored configuration by `CanbusService`.
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
- `CanbusProxy` hands the active instance to consumers that outlive a reconfiguration and
  notifies them when it is replaced.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_CANBUS` | n | Builds the subsystem. Depends on `CONFIG_CAN`; CAN FD needs `CONFIG_CAN_FD_MODE`. |
| `EERIE_LEAP_CANBUS_AUTO_DETECT_INTERVAL_MS` | 5000 | Pause between autodetect attempts. |
| `EERIE_LEAP_CANBUS_RX_QUEUE_SIZE` | 1..1024, 32 | Frames buffered between the driver callback and the RX thread. |
| `EERIE_LEAP_CANBUS_LOG_LEVEL_*` | | Log level of the subsystem. |

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
| `script_path` | Optional Lua script with `process_can_frame`; must exist when an SD card is present. |

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
group. When a configuration is stored it waits `EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS`
(default 250 ms, restarted by every further update) so that the SMP response to the request that
stored it, which may travel over the very bus being restarted, gets out first. Then, on the
configuration work queue, it stops the COM service and every service registered with
`RegisterDependentService()` (the sensor processing service and the CAN scheduler in rt_daq),
calls `CanbusService::Configure()`, and starts them again.

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_CANBUS` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION` and `EERIE_LEAP_CANBUS`. |
| `EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS` | 250 | Delay before a stored configuration is applied to the buses. |

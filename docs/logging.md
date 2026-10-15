# Logging

The logging domain (`src/domain/logging_domain/`) writes sensor readings to a file on the SD
card at a fixed interval, in MDF 4.10 by default. The MDF writer itself is the MDF subsystem
(`src/subsys/mdf/`).

## How it works

`LogWriterService` is a service with its own work queue (priority 8, stack
`EERIE_LEAP_LOG_WRITER_STACK_SIZE`). `Start()`:

1. needs a logger factory, an available SD card, a `logging_interval_ms` of at least 10 and
   `EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` free on the card;
2. builds this session's logger with the factory (`SetLoggerFactory()`), so every start, a
   restart included, writes the layout of the configurations in effect at that moment;
3. creates `EERIE_LEAP_LOG_DATA_FILES_DIR` if missing and picks a new file name
   `<prefix>_<YYYYMMDD>T<HHMMSS>Z_<rand4>.<ext>` from the start time in UTC and a random
   four-digit suffix (`log_data_20261011T093000Z_4242.mf4` by default), so names sort by start
   time; it tries up to ten suffixes when a name is taken (`LogFileNaming` builds the name);
4. discards the raw CAN frames queued before the start (the frame queues them whether or not
   logging runs), then opens the file through a seekable stream and calls the logger's
   `StartLogging()`;
5. publishes `StatusUpdated` with `IsActive = true` (see below) and schedules the first tick one
   `logging_interval_ms` later.

Each tick snapshots every sensor's latest processed reading from the `SensorReadingsFrame`, or
its latest reading when that is an error (see Invalidation bits below), logs the ones the
configuration enables, then drains the raw CAN frame queue so every frame of a `CANBUS_RAW`
sensor is logged, not only the latest one. The tick passes its own time to the logger, which
writes every reading at the reading's own timestamp, so values keep their sample time and the
frames drained in one tick keep their arrival times; the tick time stands in for a reading without
a timestamp and for a held sample written again (`log_only_new_data` off). Neither the tick nor
the logger follows a reading's sensor pointer, which belongs to the generation of sensor
processing that produced it. The tick allocates nothing: its buffers are sized by
`EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT` and `_RAW_CAN_QUEUE_SIZE`. `Start()` writes and syncs the
file header, so a card that refuses writes fails the start instead of a later tick and no empty
file is left behind. Records collect in the stream's write buffer (see
[filesystem.md](filesystem.md)); at the end of a tick, once `EERIE_LEAP_LOG_SYNC_INTERVAL_MS`
has elapsed since the last sync, the tick flushes and syncs the file, so the size and allocation
on the card cover everything up to that point and a power loss costs at most one interval plus
one buffer. `Stop()` waits for a tick in progress, then finalizes and closes the file; finalizing
syncs twice, once after the lengths and counts are patched and once after the identifier flips,
so the identifier never claims a length the card does not hold yet. It publishes `IsActive =
false` with the reason `Stopped`.

### Deadlines and raw CAN capacity

Ticks run on absolute deadlines (`K_TIMEOUT_ABS_TICKS`, so `CONFIG_TIMEOUT_64BIT` is required),
each one interval after the previous one: neither the tick's own duration nor the kernel's
rounding of a relative timeout adds to the period. A tick that runs past the next deadline (a
slow sync, a rotation) keeps the phase; the deadlines it missed are skipped and counted as
`overruns` rather than run late, back to back.

Raw CAN frames wait in the frame's queue between two drains: `EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE`
entries (32 by default), about 90 bytes each, and the writer's drain buffers are sized the same.
A frame that arrives while the queue is full is dropped and counted in `frames_dropped`, and the
writer warns once per `EERIE_LEAP_LOG_STATS_LOG_PERIOD_S` while that count grows. When a drain
fills the buffers the queue may already be dropping, so the next tick comes a quarter interval
later (10 ms at the least) and the phase goes on from there. Size the queue for
`logging_interval_ms` x the peak frame rate of the `CANBUS_RAW` sensors: at 100 ms and 1000
frames/s, 100 entries; the shortened tick absorbs a burst, not a sustained rate above that.

### Rotation and free space

With `max_log_size_mb` above 0, the tick that brings the file to that size marks it for
rotation, and the next tick, before its snapshot, finalizes and closes it and opens the next
file. The next file starts at the time of the previous tick rather than of the rotation, so the
frames and samples that arrived meanwhile are not older than the file and keep their own times;
every raw frame is in exactly one of the two files. Rotations are counted in `rotations`, and the
stats' byte and write counts cover every file of the session.

`EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` (default 16, 0 disables the check) is checked when logging
starts and before each rotation, never per tick: the free space comes from FatFs's free cluster
count, which can take a scan of the whole FAT on a large card. A start without it fails; a
rotation without it ends the session with the reason `NoSpace`, leaving the full file open for
the stop to finalize.

### Status events and failures

The writer publishes `StatusUpdated` on the `logging_events` channel (see
[infrastructure.md](infrastructure.md)) with `IsActive` and, when inactive, `Reason`
(`LoggingStatusReason`):

| Reason | When |
| --- | --- |
| `Stopped` | `Stop()` finalized and closed the file. |
| `WriteFailed` | A record or a sync did not reach the card: the stream refuses every write after one that failed or fell short (a full or removed card). |
| `NoSpace` | Less than `EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` was free at a rotation. |
| `RotationFailed` | The next file of a rotation could not be opened or its header written. |

A fifth reason, `Unavailable`, comes only from other units' status (see
[Status on other units](#status-on-other-units)). `GetReasonName()` (`event_bus/logging_status_reason.h`)
names every reason for logs and the shell, `Unavailable` included, and gives `"unknown"` for a value
from a newer unit.

A failure ends the session on the writer thread: the error is counted in `write_errors` (not for
`NoSpace`) and logged once, the failure is published, and no further tick is scheduled.
`HasFailed()` reports it until the next `Start()`. The service stays `RUNNING` without a tick:
the writer never stops its own service, because `Stop()` waits for the running tick.
`LogWriterSupervisor` stops it on the configuration work queue when the event arrives (see
[Supervision](#supervision)); that stop finalizes what it can and closes the file (a file whose
stream failed stays readable as unfinalized), then publishes `Stopped`. Events are published with
`PublishAsync()`, so subscribers run on the bus thread, never on the writer thread or inside a
transition of the service. The channel drops every publish until a bus carries it: the composition
root registers it with one of its buses, and rt_core has no bus of its own for it. The active event
goes out from inside `Start()`, before the service reports `RUNNING`.

### Restarts

A new sensors or ADC configuration restarts sensor processing, which replaces the generation
the frame's readings belong to. The supervisor's dependent (`GetDependentService()`), registered
with `SensorsController::RegisterDependentService()` (see [sensors.md](sensors.md)), stops the
writer before that generation goes and starts it again on the new one, while logging is still
wanted: the file is finalized and the next one describes the new sensors. A CAN reconfiguration
also restarts processing, without the writer; the configurations, and so the file layout, are the
same, and the drop count goes on across the frame's reset. `LoggingConfigurationManager` calls its
updated handler after `Update()` and `ApplyCborConfiguration()`, on the thread that stored the
configuration; the supervisor's handler restarts a running writer with the new interval, size
limit and sensor selection. The handler can be replaced or cleared from any thread while a store
calls it.

### Supervision

`LogWriterSupervisor` (`services/log_writer_supervisor.h`) runs the writer of a unit that logs.
It implements `ILoggingControl` (`control/i_logging_control.h`), the interface the inputs use:

| Method | Meaning |
| --- | --- |
| `RequestStart()`, `RequestStop()` | Record the wanted state and return, from any thread. The latest request wins; the outcome arrives as `StatusUpdated` on `logging_events`. |
| `IsActive()` | `IsActive` of the last `StatusUpdated` on `logging_events`; false until the first. |

A request stores the wanted state in an atomic and reschedules the supervisor's one task on the
configuration work queue. It takes no lock, so it never waits for the writer, and a bus thread or
the CDMP work queue can make it. Two requests before the task runs act once, as the later one.
Each run of the task:

1. stops a session that failed: the writer's `HasFailed()` decides, not the event, so a failure
   event that arrives after a new start stops nothing;
2. applies the pending request;
3. restarts a running writer if a logging configuration was stored, unless step 2 opened a
   session, which already uses it.

The rules, from rt_daq's controller:

- A start while running does nothing. A stop while stopped does nothing and publishes nothing.
- A start of a failed session stops it and opens a new file. A start that cannot stop it starts
  nothing.
- A failed start publishes nothing and leaves logging stopped; nothing retries it.
- A stored logging configuration restarts a running writer and leaves a stopped or failed one
  alone.
- A sensors restart starts the writer again only while logging is wanted: set by a successful
  start, cleared by a stop or a failure. A stop requested during the restart is still pending
  when the dependent is started, so the dependent does not start the writer.

| Code | Thread |
| --- | --- |
| `RequestStart()`, `RequestStop()`, `IsActive()` | Any; no lock. |
| Status handler | The bus thread that carries `logging_events`. Records the status; on a failure it reschedules the task. |
| Updated handler | The thread that stored the logging configuration (the configuration work queue for an SMP apply). Reschedules the task. |
| Every writer `Start()` and `Stop()`, the logger factory | The configuration work queue. |

Every start therefore runs on the configuration work queue: the free-space check, which may scan
the FAT of a large card, the logger's construction, the file open and the header write. Size that
queue's stack for it; rt_daq gives it 8 KiB. The dependent works only on the queue that
`SensorsController` restarts processing on, so both get the same one.

**Logger metadata.** The supervisor sets the writer's logger factory. At every start it builds
`Mdf4LoggerSensorReading` from the stored logging and sensors configurations, with:

| Field | Source |
| --- | --- |
| `device_name` | `CONFIG_BOARD` |
| `tool_id` | The supervisor's constructor (`"rt_daq"` on rt_daq). |
| `tool_version` | `EERIE_LEAP_SW_VERSION_MAJOR.MINOR.PATCH` |
| `product_id` | `EERIE_LEAP_PRODUCT_ID` |
| `hw_version`, `sw_version` | `SystemConfiguration`, packed. |
| `device_id` | The supervisor's `DeviceIdProvider` callback, 0 without one. |
| `sensors_configuration_hash` | The CRC of the stored sensors configuration, 0 when it was rejected at boot. |

The callback keeps the writer free of CDMP at compile time; a unit with a COM service returns its
CDMP device ID. The factory, and the callback with it, lives as long as the writer, so the callback
holds the COM service weakly:

```cpp
[weak_com = std::weak_ptr<CanbusComService>(canbus_com_service)] {
    const auto com = weak_com.lock();
    const auto network_info = com != nullptr ? com->GetNetworkInfo() : nullptr;

    return network_info != nullptr ? network_info->GetDeviceInfo().device_id : uint8_t{0};
}
```

**Teardown.** The destructor withdraws the status subscription, the updated handler and the task.
A handler already dispatched to it does nothing (a `DispatchGuard`), and a run in progress is
waited for. It leaves the writer as it is, for its owner to stop. The dependent and the factory
hold the writer, the wanted state and the managers, not the supervisor, so they stay harmless
after it. No request may come during or after the destructor: whoever requests keeps the
supervisor alive or goes first.

## Status on other units

A unit that logs shares its status with every other unit on the CDMP network, and a unit that
shows the status, such as the gauge, receives it as ordinary `StatusUpdated` events on its own
`logging_events` channel. Its consumers, a widget binding or a controller, cannot tell them from
local events. The status travels as a shared state record (see [cdmp.md](cdmp.md#shared-state)),
moved by the state bridge with the descriptor `LoggingNetworkState`
(`event_bus/logging_network_state.h`):

- **On the unit that logs**, `NetworkStateExporter<LoggingNetworkState>` shares the record from its
  construction, inactive and `Stopped` until the first event, then follows the writer's events.
  Its destruction withdraws the record.
- **On a unit that shows the status**, `NetworkStateImporter<LoggingNetworkState>` combines every
  other unit's record whenever one changes, and once right after its construction, and publishes
  the result on `logging_events` when it differs from what it published last.

```cpp
// rt_daq, once a bus carries logging_events: shares the writer's status.
logging_status_exporter_ = std::make_unique<NetworkStateExporter<LoggingNetworkState>>(canbus_com_service_);

// rt_gauge, once a bus carries logging_events: shows the status of the units that log.
logging_status_importer_ = std::make_unique<NetworkStateImporter<LoggingNetworkState>>(canbus_com_service_);
```

Both take the COM service as their `INetworkStateTransport` and throw `std::runtime_error` when
they cannot subscribe to the channel or register with the transport. A unit either exports or
imports the status, never both.

A bus has to carry `logging_events` first, because the channel drops every event until one does.
On an inert channel the exporter keeps sharing its initial record, and the importer publishes
nothing. The importer does not count a state it could not publish, so the next change of a record
after a bus registers the channel publishes the combined status, even if that status did not
change. If the transport refuses the exporter's record because every record slot holds another
state type, the exporter offers it again with each later event.

**The record** is `LOGGING_STATUS` (0x10), `CanbusComLoggingState` in the COM domain:

| Byte | Content |
| --- | --- |
| 0 | Flags. Bit 0: active. Bits 1-7: 0, ignored by the decoder. |
| 1 | The `LoggingStatusReason` while inactive, 0 while active. A record of one byte is inactive for the reason `Stopped`. |

Bytes after byte 1 are ignored, so fields can be appended.

**A unit's record** follows its `StatusUpdated` events, with one exception: an inactive `Stopped`
never replaces an inactive failure. The supervisor's stop that cleans up after a failure publishes
`Stopped` milliseconds later (see [Status events and failures](#status-events-and-failures)), and
the record keeps the failure until the next start. A stop of a writer that is already stopped
publishes nothing, so that cleanup is the only `Stopped` that can follow a failure.

**The combined status**, which the importer publishes, is:

- active while any unit logs;
- otherwise inactive, with the reason of the lowest device ID that reports a failure
  (`WriteFailed`, `NoSpace` or `RotationFailed`), else `Stopped`;
- inactive with the reason `Unavailable` when no unit reports a status: no logger is online, or
  this unit's CDMP node is not, since a unit that leaves `ONLINE` drops every other unit's record.

`Unavailable` (4) is only ever imported; the writer never publishes it. Code that switches over
`LoggingStatusReason` handles it too.

**Timing.** A change reaches the other units with one notification, or with a resend within
350 ms when that is lost, and otherwise with the next heartbeat (3 s) and a replay. A logger that
disappears (cable, power) leaves the combined status after the heartbeat timeout (9 s) plus up to
one validation interval (5 s). A logger that reboots keeps its last status until it leaves the
device table or sends its new record.

**Threading.** No thread is added:

- The exporter handles events on the thread of the bus that carries `logging_events` (the
  application's bus). Sharing hands the sending over to the CDMP work queue.
- The importer runs on the CDMP work queue, never blocks, and publishes with `PublishAsync()`, so
  consumers run on the importing unit's bus thread (the gauge's `AppEventBus`), as for local events.
  It keeps its buffers of every unit's record as members, off the work queue's stack.
- Imported events carry the bridge's `IMPORTED_EVENT_SOURCE_ID`, which exporters skip, so a unit
  never shares back what it imported.

The descriptor, the record and the bridge are header-only: a unit that only shows the status, such
as the gauge, builds them without `EERIE_LEAP_DOMAIN_LOGGING`, with `EERIE_LEAP_DOMAIN_CANBUS_COM`,
`EERIE_LEAP_EVENT_BUS` and a bus that carries `logging_events`.

## Loggers

`ILogger<SensorReading>` has `StartLogging(stream, start_time)`, `LogReading(time, reading)`
(`time` is the caller's current time, see above), `StopLogging()`, `GetBytesWritten()` and the
file extension. `LogReading()` throws only
`std::ios_base::failure`, when the stream refuses a record; `StopLogging()` never throws and
returns false when the file could not be finalized. One implementation exists:

**`Mdf4LoggerSensorReading`** (used by rt_daq). Built from a `LoggingConfiguration`, the sensors
and a `LoggerMetadata` (device name, tool ID and version, product ID, CDMP device ID, packed
hardware and software versions, sensors configuration hash), all copied at construction: a
reading is matched by its sensor ID hash only and its sensor pointer is never followed. It
describes the sensors as they were when it was built, which is why the service builds one
through its factory at every start. The file is written with the "unfinalized" identifier and
patched on `StopLogging()`, so a file cut by a power loss stays readable. `StartLogging()` seals
the layout into one record writer per group; each record (a CAN frame together with its payload
record) then reaches the stream's buffer in a single write, without a heap allocation, and the
header is serialized along the block chains in loops, so its stack depth does not grow with the
sensor count.

- **Time.** The master channel `Timestamp` of every group is Float64 seconds since the start
  time in the header, the record time minus the start time (0 for a reading older than the
  file). Float64 resolves microseconds over any session length; readings are stamped at the
  kernel tick's resolution (100 µs on the ESP32-S3 boards, see
  [infrastructure.md](infrastructure.md)), so frames that arrive within one tick share a time.
  Within a value group the times strictly increase: a sample stamped before the tick that last
  wrote its predecessor again (it was still being processed) is moved to 1 µs after that
  record.
- **Record IDs.** One byte while every group fits (2 x enabled sensors + 1 ≤ 255; a `CANBUS_RAW`
  sensor uses two groups), else two bytes.
- **Names.** Per enabled sensor one channel group named after the sensor ID. A value sensor's
  group holds the value channel, named after the sensor ID (Float32, unit from the metadata), and
  with `log_raw_value` on a `PHYSICAL_ANALOG` or `PHYSICAL_INDICATOR` sensor with an expression,
  the raw channel `<id>.raw`. A `CANBUS_RAW` sensor's group, also named after the sensor ID, is a
  `CAN_DataFrame` bus-event group (`BusChannel` = the source's CAN interface index + 1, `ID`,
  `IDE`, `DLC`, `Dir`, `EDL`, `BRS`, `ESI`, `DataLength`, `DataBytes` as a 64-bit VLSD offset)
  backed by a VLSD group for the payloads; asammdf's bus extraction reads it. Each frame's
  payload record is written before its frame record, so a file cut between the two ends in a
  payload no frame points at, which readers skip, rather than in a frame pointing past the end.
- **Comments.** The value channel's comment (the CAN group's, for a raw sensor) is the sensor name,
  followed by `: ` and the description when there is one. The header comment is
  `Eerie Leap sensor log` with the common properties `device_name`, `product_id`, `device_id`,
  `hw_version`, `sw_version`, `sensors_configuration_hash` and `logging_interval_ms`, in decimal;
  the file history carries the tool ID and version with the vendor `Eerie Leap`.
- **Conversion.** The raw channel gets an algebraic conversion only when a reader evaluates it
  the way the expression engine does: the only identifier is `x`, and the expression uses
  nothing but `+ - * /`, parentheses, decimal numbers (no leading zero, which numexpr rejects)
  and `sin cos tan sqrt abs log`. The formula is the expression with `x` renamed to `X`
  token-wise and `a ^ n` written as the product `(a * ... * a)` when `a` is a single token and `n`
  a whole number from 1 to 4 (asammdf's numexpr has no `pow()`; other powers are not
  converted): `x * 4 + 1.6` becomes `X * 4 + 1.6`, `x ^ 2 * 0.5` becomes `(X * X) * 0.5`. Any
  other expression goes into the raw channel's comment as `expression: <text>`.
- **Invalidation bits.** The value channel and the raw channel each have an invalidation bit
  (bits 0 and 1 of one byte after the record's data, so with a one-byte record ID value records
  are 14 bytes, 18 with the raw value; the master channel and CAN groups have none). A reading
  with an error (`HasError()`) or without a value is written, not skipped: the value field is 0
  and the value channel's bit is set, so the stale value an error reading may still carry never
  looks valid. A missing raw value is written as 0 with only the raw channel's bit set; an error
  reading's raw value, when it has one, stays valid (the input was measured, a later step
  failed). The service logs each sensor's latest processed reading, or its latest reading when
  that is an error (`SensorReadingsFrame::SnapshotLoggableReadings()`), so a sensor whose readings
  fail shows invalid samples from the first failed one until it recovers, not its last good value
  held.
- **Invalid samples in asammdf** (8.8). `MDF.get(name)` honours the bits by dropping the invalid
  samples, timestamps included (`invalidation_bits` is `None`); `get(name,
  ignore_invalidation_bits=True)` returns every sample, the 0 placeholders included, with the
  bits in `Signal.invalidation_bits` (`True` = invalid; `validate()` drops those samples).
  `to_dataframe()` interpolates across the dropped samples unless `use_interpolation=False`,
  which leaves NaN there, and `MDF(path, ignore_invalidation_bits=True)` reads the placeholders
  as values. The GUI opens files with the bits honoured unless its setting says otherwise.
- **Readings.** The tick offers every sensor's latest sample each time. A sample whose
  timestamp was already written is skipped with `log_only_new_data`, and otherwise written
  again at the tick time (sample and hold at the logging interval); a reading without a
  timestamp is always written. An invalid reading follows the same rules: its own timestamp
  makes it a new or a held sample, and a held one is written invalid again. Raw CAN frames ignore
  the flag: every queued frame is written.

## Stored configuration

Schema (`logging_config.cddl`), file `config/logging_config.cbor`, type 5 in the `config`
group:

```
CborSensorLoggingConfig = [ sensor_id_hash: uint, is_enabled: bool, log_raw_value: bool, log_only_new_data: bool ]
CborLoggingConfig = [ logging_interval_ms: uint, max_log_size_mb: uint, sensor_configs: [*CborSensorLoggingConfig] ]
```

| Field | Default | Rules and meaning |
| --- | --- | --- |
| `logging_interval_ms` | 100 | ≥ 10. Period of the writer's tick. |
| `max_log_size_mb` | 10 | ≤ 1024. A file that reaches this size is finalized and the next one opened (see [Rotation and free space](#rotation-and-free-space)); 0 never rotates. |
| `sensor_configs[].sensor_id_hash` | | 32-bit hash of the sensor ID (see [smp.md](smp.md)). A sensor without an entry is not logged. |
| `is_enabled` | false | Log this sensor. A sensor disabled in the sensors configuration (see [sensors.md](sensors.md#sensor-model)) is not logged either way. |
| `log_raw_value` | false | Also log the value before the expression (MDF4: the `<id>.raw` channel). |
| `log_only_new_data` | false | Skip a tick when the reading's timestamp has not changed since the last record of this sensor; off, the held sample is written again at the tick time. A reading without a timestamp is always logged. Does not apply to `CANBUS_RAW` sensors, whose every frame is logged. |

The default configuration logs nothing.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_LOGGING` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION` and `EERIE_LEAP_DOMAIN_SENSOR` (the readings frame, the sensors configuration the loggers describe); selects `EERIE_LEAP_EVENT_BUS`, `EERIE_LEAP_MDF` and `EERIE_LEAP_DOMAIN_SYSTEM` (the product ID and versions in the log header). |
| `EERIE_LEAP_LOG_WRITER_STACK_SIZE` | 8192 | Stack of the writer's work queue. A rotation finalizes a file and writes the next header on it (3648 bytes on the ESP32-S3), and a failed write unwinds an exception on it (rt_daq uses 6144). |
| `EERIE_LEAP_LOG_SYNC_INTERVAL_MS` | 0..3600000, 1000 | How often a tick flushes and syncs the log file; bounds what a power loss can take. 0 syncs only when logging stops. |
| `EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` | 0..1048576, 16 | Free space needed to start logging and to rotate to the next file. 0 disables the check. |
| `EERIE_LEAP_DOMAIN_LOGGING_STATS` | n | Logs the writer's counters once per period: ticks, readings, frames, drops, bytes, stream writes, syncs, rotations, overruns, errors, tick duration. The counters are always kept (`GetStats()`). |
| `EERIE_LEAP_LOG_STATS_LOG_PERIOD_S` | 1..3600, 10 | Period of the statistics line, and the shortest interval between two dropped-frame warnings. |
| `EERIE_LEAP_LOG_DATA_FILES_DIR` | `"data_logs"` | Directory of the log files on the SD card. |
| `EERIE_LEAP_LOG_DATA_FILE_PREFIX` | `"log_data"` | File name prefix; empty drops the prefix and the underscore. |
| `EERIE_LEAP_MDF` | n | Builds the MDF 4 writer; selected by `EERIE_LEAP_DOMAIN_LOGGING`, and usable without it. |

## Wiring

rt_core has no logging controller yet; a unit that logs builds the parts:

1. A bus that carries `logging_events`, registered by the composition root before anything
   publishes on it: the channel drops every publish until then.
2. `LoggingConfigurationManager` on `config/logging_config.cbor` with the configuration work
   queue, registered as type 5.
3. `LogWriterService(sd_fs, logging_manager, time_service, readings_frame)`, initialized.
4. `LogWriterSupervisor(config_work_queue, log_writer_service, logging_manager, sensors_manager,
   tool_id, device_id_provider)`, after `SensorsController::Initialize()`, which builds the sensors
   manager, and with the queue `SensorsController` was given; then its `Initialize()`, which warns
   when no bus carries `logging_events` yet.
5. Only if that succeeded, `SensorsController::RegisterDependentService(supervisor->GetDependentService())`,
   before processing starts.
6. The inputs (the CDMP command, the shell, an autostart) call `ILoggingControl`, and a display
   subscribes to `logging_events`.
7. A unit that shares its status with other units also creates the exporter described under
   [Status on other units](#status-on-other-units).

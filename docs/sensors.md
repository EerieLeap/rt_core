# Sensors

The sensor domain (`src/domain/sensor_domain/`) turns a list of sensor definitions into
readings: it samples ADC and GPIO channels, decodes CAN frames, evaluates expressions, runs
scripts, and keeps the latest value of every sensor in a frame that logging, CAN sending, BLE
live data, expressions and the sensor events publisher read from.

## Sensor model

A sensor is an `id`, an `is_enabled` flag, metadata and a configuration:

| Field | Rules |
| --- | --- |
| `id` | Letters, digits and `_`, not starting with a digit. Unique, and its 32-bit hash must be unique too (readings, logging and clients refer to sensors by the hash). |
| `is_enabled` | Default true. A disabled sensor keeps its configuration and its slot in the readings frame, but nothing reads it: it gets no reader, CAN route, expression or script, never has a reading, and is not logged. It is validated like an enabled sensor, except that it may reference a disabled sensor or CAN message. |
| `metadata.name`, `metadata.unit` | At most 32 bytes (UTF-8). |
| `metadata.description` | At most 128 bytes. |
| `configuration.type` | One of the types below. |
| `configuration.channel` | Global ADC channel or GPIO channel index; only for types that take one. |
| `configuration.connection_string` | CAN source, `bus_channel/frame_id[/signal_name]`; only for CAN types. Each number is decimal (a leading `0` too) or hexadecimal after `0x`, the bus channel at most 255 and the frame ID at most `0x1FFFFFFF`; the signal name is the rest of the text. Written in decimal. |
| `configuration.sampling_rate_ms` | Polling period; see the table. |
| `configuration.interpolation_method`, `calibration_table` | `LINEAR` (1) or `CUBIC_SPLINE` (2) over 2 to 50 points `{voltage, value}` of finite numbers with distinct voltages; only for `PHYSICAL_ANALOG`, where both are required. |
| `configuration.expression` | Formula over `x`, for the types that have an input, and other sensor IDs; see [scripting.md](scripting.md). |
| `configuration.script_path` | Lua script on the SD card; see [scripting.md](scripting.md). |
| `configuration.debounce_ms` | 0 to 60000, default 0: a change of the input counts at once. Indicator types only; see [Indicator conditioning](#indicator-conditioning). |
| `configuration.input_mode` | `Level` (0, default): the value follows the input. `Toggle` (1): each press flips the value, which holds. Indicator types only. |

### Types

| Type | ID | Source | Channel | Connection string | Interpolation | Expression | Sampling rate | Updated |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `PHYSICAL_ANALOG` | 1 | ADC | required | no | required | optional, `x` = interpolated value | required | polled |
| `VIRTUAL_ANALOG` | 2 | expression | no | no | no | required, no `x` | optional | polled with a rate, else after an input commits |
| `PHYSICAL_INDICATOR` | 3 | GPIO | required | no | no | optional, `x` = 0/1 | optional | polled with a rate, else on every edge |
| `VIRTUAL_INDICATOR` | 4 | expression | no | no | no | required | optional | as `VIRTUAL_ANALOG`; value stored as 0/1 |
| `CANBUS_RAW` | 5 | CAN frame | no | `bus/frame` | no | not allowed | not allowed | on every frame; the frame is the reading |
| `CANBUS_ANALOG` | 6 | CAN signal | no | `bus/frame/signal` | no | optional, `x` = decoded signal | not allowed | on every frame |
| `CANBUS_INDICATOR` | 7 | CAN signal | no | `bus/frame/signal` | no | optional | not allowed | on every frame; value 0/1 |
| `USER_ANALOG` | 8 | Lua `create_sensor_value` | no | no | no | optional, `x` = script value | required | polled |
| `USER_INDICATOR` | 9 | Lua | no | no | no | optional | required | polled; value 0/1 |

Only the indicator types take a `debounce_ms` other than 0 or the `Toggle` mode.

A virtual sensor without a sampling rate must reference at least one sensor, otherwise it would
never update; its expression cannot use `x`, which it does not have. An enabled sensor's
expression cannot reference a disabled sensor, which never has a value. A `USER_ANALOG` or
`USER_INDICATOR` sensor needs no script: without `create_sensor_value` its readings have no value
of their own, and other scripts set it with `update_sensor_value()`. Values and channels are
checked against the unit: an ADC or GPIO channel must exist (the error names the channels the
unit has), and with an SD card present a `script_path` must exist.

When a client applies a configuration (the SMP `config` group), each CAN sensor's bus must also be
configured in the CAN bus configuration the unit runs, and a signal sensor's frame and signal must
be defined on it, the message enabled when the sensor is (see [canbus.md](canbus.md)); a raw
sensor reads every frame with its ID, so it needs only the bus. A stored configuration is not
checked against it again: after a CAN bus change it still loads, and a sensor whose signal is
gone or whose message is disabled logs so when the pipeline is built and stays silent.

The traits behind this table are in `models/sensor_type_traits.h`; the rules are enforced by
`configuration/parsers/sensor_validator.cpp`, the CAN bus check by
`SensorValidator::ValidateCanbusSources()`, which `SensorsConfigurationManager` runs on a
client's configuration.

## Processing

### Pipeline

Every reading, whatever its source, goes through the same `ReadingPipeline`:

```
reader / CAN route / GPIO edge
   └─► ExpressionProcessor ─► ScriptProcessor ─► IndicatorConditioner ─► commit to SensorReadingsFrame ─► dependents
```

1. **Reader** produces a reading with `status = RAW` (or `INTERPOLATED` for analog) and the tick's
   timestamp. A source that fails gives `READER_FAILED`.
2. **ExpressionProcessor.** For expression sources it evaluates the expression; for the others it
   evaluates it with `x` = the current value when one is configured. It needs every referenced
   sensor to have a value (`INPUT_UNAVAILABLE` otherwise) and rejects NaN. Indicators are coerced
   to 0 or 1. Status becomes `EXPRESSION_EVALUATED`.
3. **ScriptProcessor** calls `post_process_sensor_value(sensor_id, value)` when the sensor has a
   script that defines it; a number replaces the value.
4. **IndicatorConditioner**, for an indicator with conditioning: passes the reading on with the
   conditioned value, or holds a change until it settles (see
   [Indicator conditioning](#indicator-conditioning)).
5. **Commit.** The reading is copied into the frame and marked `PROCESSED`. A failed reading is
   committed with its error and `status = ERROR`; consumers see the error, and its dependents keep
   their previous values.
6. **Dependents.** Every `DEPENDENT` sensor whose expression reads this one, directly or
   transitively, is evaluated once, in processing order, with the same timestamp, through steps 2
   to 5.

Processing order is the topological order of expression references, computed when the
configuration is parsed (`SensorsOrderResolver`); a cycle is a validation error.

### Indicator conditioning

An indicator of any source (`PHYSICAL_INDICATOR`, `VIRTUAL_INDICATOR`, `CANBUS_INDICATOR`,
`USER_INDICATOR`) can be debounced and can act as a switch. `IndicatorConditioner`
(`processors/indicator_conditioner.h`) does both. The builder gives one only to an indicator whose
`debounce_ms` or `input_mode` is not the default, so the defaults cost nothing.

- **Input.** The processed value: 0 is off, any other number is on, a `post_process_sensor_value`
  result included. The committed value is 0 or 1; `raw_value` keeps what the source read.
- **Debounce settles, it does not drop.** A change of the input counts once the input has held it
  for `debounce_ms`. An input back at the committed one before then cancels the change, and
  further readings of the new value do not restart the window. The value therefore ends on the
  real level, a bounce never shows, and a pulse shorter than the window is ignored. With
  `debounce_ms` 0 a change counts at once.
- **When.** The conditioner's work item on the processing work queue commits the change at the
  end of the window, so an edge source that sends nothing after the edge settles too. A polled
  sample after the end commits it as well. The commit carries the time the change settled and goes
  through the commit step of any reading, dependents included.
- **Held readings are not committed.** The frame only shows conditioned values, and a sensor that
  reads a conditioned indicator sees only settled ones. A `VIRTUAL_INDICATOR` with conditioning
  holds its own changes; until one settles, the sensors that read it are evaluated with its old
  value.
- **Errors.** A reading with an error, or without a value, is committed as it is and leaves the
  window and the switch alone.
- **First value.** The first reading of a generation is committed at once: there is nothing to
  settle against. In `Toggle` mode it sets the input without a flip, so a button held while
  processing starts is not a press.
- **Toggle.** Each settled 0 → 1 flips the value, which holds. A settled 1 → 0 is committed with
  the value unchanged: every reading the conditioner does not hold is committed, so the reading's
  timestamp and `raw_value` follow the input, and a polled switch refreshes its reading on every
  sample. The log writes `raw_value` only as `<id>.raw`, with `log_raw_value` on a
  `PHYSICAL_INDICATOR` that has an expression (see [logging.md](logging.md)). The frame, the log,
  the publisher, widgets and expressions all see the switch.
- **A switch across restarts.** When processing starts, the value of every `Toggle` sensor is
  committed before its input has a reading, with its dependents. A restart (a sensors, ADC or CAN
  apply) keeps the switch of a sensor whose id, type, `debounce_ms` and `input_mode` are
  unchanged. A change to any of these four starts it off. A change to the channel, connection
  string, sampling rate, expression or script keeps it. The switch comes from the last generation
  that ran: a sensor missing from it, or not a `Toggle` in it, starts off, and a failed start
  changes nothing. A reboot starts every switch off. A change held when processing stops is
  dropped.
- **Pause.** A pause (ADC calibration, BLE pairing) stops the window of a held change with the
  sources, since none can confirm or cancel it until `Resume()`, and `Resume()` runs what was left
  of it. The first reading after `Resume()` confirms or cancels the change as any reading does: a
  GPIO input is read again at `Resume()`, so a press released during the pause is ignored, and a
  polled sensor has its next sample. A source that sends nothing more, such as a CAN signal,
  settles at the end of the window. A switch keeps its value.
- **Lua.** `update_sensor_value()` writes the frame directly (`UpdateReadingValue`), past the
  debounce and the switch; the sensor's next commit replaces what it wrote.
- **Latency and cost.** A window delays every consumer of the sensor by its length: keep a
  button's short (20–30 ms) and CAN signals at 0. A conditioner is a delayable work item and a copy
  of the held reading, built with the generation on the default heap, since the timer walks it; a
  reading allocates nothing.
- **GPIO.** The GPIO subsystem does not debounce. No time window drops edges. Edges are coalesced
  to the newest level, so the sensor always ends on the real level, and its `debounce_ms` settles
  a bouncing contact (see [gpio.md](gpio.md)).

### Indicators as logging triggers

A logging trigger binds to an indicator's `DataUpdated` event on `sensor_events` (see
[logging.md](logging.md#triggers)), and the indicator's conditioning decides what the trigger sees,
so the trigger itself has no mode:

- `Toggle` makes a momentary button a switch, and `debounce_ms` settles its contact. A widget bound
  to the same sensor shows the same switch as the trigger acts on.
- `Level` follows a switch, a CAN signal or an expression as it is. A debounce keeps a short dip
  from stopping a session.
- The trigger starts logging on 0 to 1 and stops it on 1 to 0, and reads a first value of 1 as a
  start, so a switch that is on at boot needs no new edge. A `Toggle` switch keeps its value across
  a sensors apply (see [Generations and restarts](#generations-and-restarts)), so the trigger sees
  no change and requests nothing, and logging continues (see [logging.md](logging.md#restarts)).
- A change to a `Toggle` sensor's type, `debounce_ms` or `input_mode` starts its switch off, which
  the trigger reads as 1 to 0 and stops logging (a change to `Level` waits for the sensor's first
  reading). A change to its id stops nothing, and neither does removing or disabling it: a trigger
  selects its sensor by id, so it hears nothing more and keeps its last state, and a trigger on the
  new id reads the first 0 as no request. Logging goes on in a new file (see
  [logging.md](logging.md#triggers)).

| Example | Sensor | Trigger |
| --- | --- | --- |
| The gauge's button | `PHYSICAL_INDICATOR` on GPIO 0, `debounce_ms` 30, `Toggle` | `Sensors` / `DataUpdated` / `Value`, selector `logging_button` |
| A switch on a DAQ | `PHYSICAL_INDICATOR` on GPIO 1, `debounce_ms` 30, `Level` | the same, selector `logging_switch` |
| Log while the ignition is on | `CANBUS_INDICATOR`, no debounce | the same, selector `ignition` |
| Log while moving, ignoring a dip shorter than 2 s | `VIRTUAL_INDICATOR`, expression `speed > 5`, `debounce_ms` 2000 | the same, selector `moving` |

The sensor and the trigger are two stored configurations that must agree: a trigger on a sensor
that does not exist does nothing and is logged when the triggers are built.

### Polled sensors: rate groups

`ProcessingSchedulerService` puts every polled sensor into the rate group of its
`sampling_rate_ms` and runs all groups on one delayable work item of the processing work queue:

- A group's period is rounded up to whole system ticks (a warning says so when it is not a whole
  number). A period below 1 ms is rejected.
- Deadlines are absolute, so a group keeps its phase whatever a tick costs. A tick that runs past
  a deadline skips the missed ones and counts them as overruns.
- A tick runs every due group, fastest first. For each group it first acquires the group's ADC
  channels (one `Sample()` per ADC covering every channel the group reads, with the configured
  `samples`), then reads the sensors in processing order under one timestamp, so a derived sensor
  in the group sees this tick's inputs. Sensors on the same channel share one conversion.
- `GetRateGroupStats()` returns ticks, overruns, last and longest tick per group. With
  `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS` one line per group is logged every
  `_STATS_LOG_PERIOD_S` with the delivered period (average, minimum, maximum) and the latest
  volts of each ADC channel the group samples.
- A sensor whose reader cannot be built (no ADC on the unit, a channel without calibration) is
  logged and left out; the rest of the group runs.

### Event-driven sensors

`ProcessingIsrService` runs the sensors without a sampling rate:

- **CAN.** One `CanFrameRouter` registers one bus handler per route (bus, frame ID). The CAN RX
  thread copies a frame into the route's pending slot and submits one work item; frames arriving
  faster than the processing queue drains are coalesced per route, newest wins, nothing is
  allocated per frame. The work item decodes every route with a pending frame: one reading per
  bound sensor, a signal decoded per the CAN configuration, or the frame itself for `CANBUS_RAW`.
- **GPIO.** One `GpioSensorReader` per sensor subscribes to both edges, coalesces to the newest
  level, and seeds the frame with the current level at start. No time window drops edges, so the
  sensor always ends on the real level, and its `debounce_ms` settles a bouncing contact.

### Readings frame

`SensorReadingsFrame` holds the latest reading of every configured sensor in one slot per sensor,
laid out when a configuration generation is built. Commits and snapshots are a few word copies
under one mutex and never allocate; value slots have stable addresses, which expressions bind to.

| Call | Use |
| --- | --- |
| `TryGetReading(id / hash / slot)`, `TryGetReadingValue(...)`, `GetReadingValues(hashes, values)` | Latest reading or value of a sensor. |
| `SnapshotProcessedReadings(span)`, `TakeProcessedReadings(span)` | All processed readings; `Take` also clears the "updated" mark. Buffers are sized with `SensorLimits::kMaxCount`. |
| `TryGetCanFrame(hash, frame)` | Latest frame of a `CANBUS_RAW` sensor. |
| `TakeRawCanFrames(readings, frames)` | Every raw frame since the last take, oldest first, from a queue of `kRawCanQueueSize` entries; overflow is counted. For the log writer. |
| `UpdateReadingValue(hash, value)` | What `update_sensor_value()` in Lua calls; it bypasses indicator conditioning. |
| `MarkProcessedReadingsUpdated()` | Marks every processed reading as not yet taken, so the next `Take` returns each once more. Behind `SensorEventsPublisher::Replay()`. |

### Generations and restarts

`SensorsProcessingService::Start()` builds a **generation** from the current sensors
configuration and a snapshot of the ADC configuration: the frame's slots, one `SensorRuntime` per
sensor (interpolator, compiled expression bound to the frame, loaded script, dependents; none of
them, and update method `NONE`, for a disabled sensor), then the rate groups and event sources.
Everything a sample needs is allocated here, once. The switches of the `Toggle` indicators are
committed before the sources start.
`Stop()` tears the sources down, cancels held indicator changes, keeps every switch for the next
`Start()`, clears the frame and drops the generation.

A new sensors or ADC configuration takes effect through a restart: the `SensorsController`
registers update handlers on both managers that queue `Stop(); Start();` on the configuration
work queue. A restart that fails (a script that no longer loads, a channel that disappeared)
leaves the service stopped and logged; the next valid configuration starts it again. The service
is pausable: the calibration service and BLE pairing pause it and resume it. A pause tears the
sources down and stops the windows of held indicator changes until `Resume()`; the generation and
its switches stay.

Services that read the frame or the sensors of the running generation register with
`SensorsController::RegisterDependentService()` before `Start()`. A restart stops the registered
services that are running before processing stops, and starts exactly those again once
processing has been started, whether or not that start succeeded; a dependent that does not stop
or does not come back is logged and does not fail the restart. The log writer joins through
`LogWriterSupervisor::GetDependentService()`, so a sensors apply while logging finalizes the file
and opens the next one for the new sensors (see [logging.md](logging.md#restarts) and
[Supervision](logging.md#supervision)).

## Sensor events

`SensorEventsPublisher` (`services/sensor_events_publisher.h`, built with `EERIE_LEAP_EVENT_BUS`)
turns the frame into events. Every `EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS`, on a work
queue thread of its own, it takes the readings processed since its last tick
(`TakeProcessedReadings`) and publishes `SensorEventType::DataUpdated` on `sensor_events` for each
one with a value: `SensorId` is the id hash (`uint32_t`), `Value` the value (`float`).

- **One per unit.** Taking clears the marks, so a second publisher would split the readings. Each
  application constructs one with the shared frame and starts it before `SensorsController::Start()`.
  A subscriber that comes later calls `Replay()` to see the current state, as `LoggingController`
  does.
- **The latest value per tick.** A sensor committed several times within one period is published
  once, with its latest value; changes across ticks arrive in order. A pulse shorter than the
  period can be missed; a switch (a held value) cannot.
- **Only what someone listens to.** A reading is published only if a subscriber accepts it
  (`EventChannel::Accepts(event)`, see [infrastructure.md](infrastructure.md#event-bus-eerie_leap_event_bus));
  others are taken and dropped. On a unit without widgets (rt_daq) that leaves only the sensors its
  logging triggers bind to, so a full sensor load never floods the channel, which sheds events
  under load; on the gauge it leaves the sensors its widgets and its logging triggers bind to. The
  answer is cached per sensor until the channel's subscriptions change, so the filters run once per
  sensor and change, not once per reading. A filter on this channel must therefore decide by
  `SensorId`, as binding selectors do. A subscriber without a filter receives every sensor.
- **A filter must not throw.** One that throws withholds its sensor from every subscriber. When
  the publisher asks it, the throw counts as a rejection until the subscriptions change and is
  logged once. When a subscriber that accepts is asked first, the throw happens on the bus thread
  at every dispatch. The publisher catches the throw only if its stack can unwind it (the 4 KiB
  default overflowed on qemu_cortex_a53).
- **Replay.** `Replay()` marks every processed reading as updated (`MarkProcessedReadingsUpdated()`),
  so the next tick publishes the latest value of every accepted sensor once. Safe from any thread;
  a consumer that has just subscribed calls it to see the current state rather than wait for the
  next change. `LoggingController` calls it after it builds its triggers, at `Initialize()` and
  after each rebuild, when at least one trigger subscribed, so a trigger sees a switch that is
  already on. Every subscriber receives the replay, widgets included, which re-apply their values.
  It is reliable because a tick reads the
  subscription generation after each take: a subscription made before the call to `Replay()` has
  moved the generation by then, so a sensor cached as rejected is asked again.
- **Lifecycle.** A `ServiceBase`: `Initialize()` creates the thread, `Start()` the ticks; it is
  pausable, and `Stop()` (also run by the destructor) waits for a tick in progress.
- **Memory.** The readings buffer (`kMaxCount` readings) and the acceptance cache (`kMaxCount`
  answers, 8 bytes each) are part of the object, so they come with its owner's allocation.
  `Initialize()` allocates the thread's stack. A tick never allocates.

## Stored configuration

Schema (`sensors_config.cddl`):

```
CborSensorMetadataConfig = [ name: tstr, unit: tstr, description: tstr ]
CborSensorCalibrationDataMap = { 2*50(float32 => float32) }
CborSensorConfigurationConfig = [
  type: uint, sampling_rate_ms: int, interpolation_method: uint, ? channel: uint,
  connection_string: tstr, script_path: tstr, ? calibration_table: CborSensorCalibrationDataMap, ? expression: tstr,
  debounce_ms: uint, input_mode: uint ]
CborSensorConfig = [ id: tstr, is_enabled: bool, metadata: CborSensorMetadataConfig, configuration: CborSensorConfigurationConfig ]
CborSensorsConfig = [ sensors: [*CborSensorConfig] ]
```

`is_enabled`, `debounce_ms` and `input_mode` are required: a file stored before any of them existed
does not decode, so it is kept and the unit runs on the default (see
[configuration.md](configuration.md#configuration-managers)) until the configuration is written
again. The encoder always writes them; their defaults (0 and `Level`) are in the model, so a sensor
built in code gets them. An unknown `input_mode` is a validation error.
`sampling_rate_ms` ≤ 0 means none. `interpolation_method` 0 means none. The decoder takes any
number of calibration points and repeated voltages; the validator holds the table to 2 to 50
points with distinct voltages, and only then is it ordered by voltage. The stored order is not
significant: sensors are re-sorted into processing order on load. The default configuration
is an empty list. File: `config/sensors_config.cbor`, type 4 in the `config` group.

Example through the API (the application's test setup, trimmed):

```cpp
auto sensor_1 = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_1");
sensor_1->metadata.name = "Sensor 1";
sensor_1->metadata.unit = "km/h";
sensor_1->configuration.type = SensorType::PHYSICAL_ANALOG;
sensor_1->configuration.channel = 0;
sensor_1->configuration.sampling_rate_ms = 50;
sensor_1->configuration.interpolation_method = InterpolationMethod::LINEAR;
sensor_1->configuration.calibration_table = { {0.0, 0.0}, {5.0, 100.0} };

auto sensor_5 = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_5");
sensor_5->configuration.type = SensorType::VIRTUAL_INDICATOR;
sensor_5->configuration.expression = "sensor_1 < 400";      // no rate: updates after sensor_1

auto sensor_6 = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_6");
sensor_6->configuration.type = SensorType::CANBUS_ANALOG;
sensor_6->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetExtPmr(), 1, 790, "RPM");

auto sensor_7 = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_7");
sensor_7->configuration.type = SensorType::PHYSICAL_INDICATOR;
sensor_7->configuration.channel = 0;                                // no rate: every edge
sensor_7->configuration.debounce_ms = 30;
sensor_7->configuration.input_mode = IndicatorInputMode::Toggle;    // each press flips it

sensors_configuration_manager->Update({sensor_1, sensor_5, sensor_6, sensor_7});
```

## Kconfig

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_SENSOR` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION`, `EERIE_LEAP_ADC` and `EERIE_LEAP_CANBUS`; selects `EERIE_LEAP_DOMAIN_SCRIPT`, `EERIE_LEAP_DOMAIN_ADC` and `EERIE_LEAP_DOMAIN_CANBUS` (CAN sensors). |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE` | 8192 | Stack of the processing work queue, where every tick, decode and script call runs. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY` | 6 | Its priority. rt_daq uses 5, above the display and CAN scheduler threads. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS` | n | Periodic rate group statistics log; a measurement aid, off in production. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS_LOG_PERIOD_S` | 1..3600, 10 | Seconds between statistics lines. |
| `EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS` | 1..10000, 30 | Delay between two publisher ticks. This and the next two need `EERIE_LEAP_EVENT_BUS`. |
| `EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_STACK_SIZE` | 4096 | Stack of its work queue, where the subscribers' filters also run. A filter that throws is caught, but with one the thread peaked at 1.5 to 7.8 KiB on the QEMU test boards. rt_gauge's ESP32-S3 board uses 2560. |
| `EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PRIORITY` | 6 | Its priority. |
| `EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT` | 1..65535, 64 | Most sensors a configuration may hold; sizes every snapshot buffer. |
| `EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE` | 1..1024, 32 | Raw CAN frames queued for the log writer; about 90 bytes each, allocated only while an enabled `CANBUS_RAW` sensor exists. |

## Wiring

`SensorsController` (`EERIE_LEAP_CONTROLLER_SENSORS`; depends on `EERIE_LEAP_DOMAIN_SENSOR`,
`EERIE_LEAP_DOMAIN_CANBUS`, `EERIE_LEAP_GPIO`, `EERIE_LEAP_RANDOM`, `EERIE_LEAP_FS`) takes the
internal filesystem, the configuration work queue, the configuration service, the time service,
the shared `SensorReadingsFrame`, the CAN service, the GPIO, and optionally the ADC manager, the
ADC configuration manager and the SD service. It creates the configuration manager (validating
channels against the GPIO and ADC channel counts, and a client's CAN sensors against the CAN
service's configuration), the reader factories and the processing service, registers type 4, and
wires the restart handlers. `Start()` starts processing; `RegisterDependentService()` adds a
service to stop and start around a restart (see above). `GetCapabilities()` gives the channel
counts and `EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT`, which rt_daq passes to the `ManagementController`
for the device `INFO` (see [smp.md](smp.md)). The
application also registers the processing service with the ADC controller (to be paused during
calibration) and the CAN controller (to be restarted on CAN reconfiguration). The application
constructs the `SensorEventsPublisher` itself, with the same frame (see
[Sensor events](#sensor-events)), and passes it to `LoggingController`, which replays after it
rebuilds its triggers (see [logging.md](logging.md#wiring)).

Measured on the ESP32-S3 (application notes): an ADC reading with 40 samples takes about
1.2 ms, 3.3 ms with an expression; a GPIO reading 130 µs, 1.9 ms with an expression.

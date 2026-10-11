# Sensors

The sensor domain (`src/domain/sensor_domain/`) turns a list of sensor definitions into
readings: it samples ADC and GPIO channels, decodes CAN frames, evaluates expressions, runs
scripts, and keeps the latest value of every sensor in a frame that logging, CAN sending, BLE
live data and expressions read from.

## Sensor model

A sensor is an `id`, metadata and a configuration:

| Field | Rules |
| --- | --- |
| `id` | Letters, digits and `_`, not starting with a digit. Unique, and its 32-bit hash must be unique too (readings, logging and clients refer to sensors by the hash). |
| `metadata.name`, `metadata.unit` | At most 32 characters. |
| `metadata.description` | At most 128 characters. |
| `configuration.type` | One of the types below. |
| `configuration.channel` | Global ADC channel or GPIO channel index; only for types that take one. |
| `configuration.connection_string` | CAN source, `bus_channel/frame_id[/signal_name]`; only for CAN types. |
| `configuration.sampling_rate_ms` | Polling period; see the table. |
| `configuration.interpolation_method`, `calibration_table` | `LINEAR` (1) or `CUBIC_SPLINE` (2) over 2 to 50 points `{voltage, value}`; only for `PHYSICAL_ANALOG`, where both are required. |
| `configuration.expression` | Formula over `x` and other sensor IDs; see [scripting.md](scripting.md). |
| `configuration.script_path` | Lua script on the SD card; see [scripting.md](scripting.md). |

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

A virtual sensor without a sampling rate must reference at least one sensor, otherwise it would
never update. A CAN signal sensor's `bus/frame/signal` must name a signal defined in the CAN bus
configuration of that bus (see [canbus.md](canbus.md)); a raw sensor only needs the bus and
frame ID. Values and channels are checked against the unit: an ADC or GPIO channel must exist,
and with an SD card present a `script_path` must exist.

The traits behind this table are in `models/sensor_type_traits.h`; the rules are enforced by
`configuration/parsers/sensor_validator.cpp`.

## Processing

### Pipeline

Every reading, whatever its source, goes through the same `ReadingPipeline`:

```
reader / CAN route / GPIO edge
   └─► ExpressionProcessor ─► ScriptProcessor ─► commit to SensorReadingsFrame ─► dependents
```

1. **Reader** produces a reading with `status = RAW` (or `INTERPOLATED` for analog) and the tick's
   timestamp. A source that fails gives `READER_FAILED`.
2. **ExpressionProcessor.** For expression sources it evaluates the expression; for the others it
   evaluates it with `x` = the current value when one is configured. It needs every referenced
   sensor to have a value (`INPUT_UNAVAILABLE` otherwise) and rejects NaN. Indicators are coerced
   to 0 or 1. Status becomes `EXPRESSION_EVALUATED`.
3. **ScriptProcessor** calls `post_process_sensor_value(sensor_id, value)` when the sensor has a
   script that defines it; a number replaces the value.
4. **Commit.** The reading is copied into the frame and marked `PROCESSED`. A failed reading is
   committed with its error and `status = ERROR`; consumers see the error, and its dependents keep
   their previous values.
5. **Dependents.** Every `DEPENDENT` sensor whose expression reads this one, directly or
   transitively, is evaluated once, in processing order, with the same timestamp.

Processing order is the topological order of expression references, computed when the
configuration is parsed (`SensorsOrderResolver`); a cycle is a validation error.

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
  level, and seeds the frame with the current level at start.

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
| `UpdateReadingValue(hash, value)` | What `update_sensor_value()` in Lua calls. |

### Generations and restarts

`SensorsProcessingService::Start()` builds a **generation** from the current sensors
configuration and a snapshot of the ADC configuration: the frame's slots, one `SensorRuntime` per
sensor (interpolator, compiled expression bound to the frame, loaded script, dependents), then
the rate groups and event sources. Everything a sample needs is allocated here, once.
`Stop()` tears the sources down, clears the frame and drops the generation.

A new sensors or ADC configuration takes effect through a restart: the `SensorsController`
registers update handlers on both managers that queue `Stop(); Start();` on the configuration
work queue. A restart that fails (a script that no longer loads, a channel that disappeared)
leaves the service stopped and logged; the next valid configuration starts it again. The service
is pausable: the calibration service and BLE pairing pause it and resume it.

## Stored configuration

Schema (`sensors_config.cddl`):

```
CborSensorMetadataConfig = [ name: tstr, unit: tstr, description: tstr ]
CborSensorCalibrationDataMap = { 2*50(float32 => float32) }
CborSensorConfigurationConfig = [
  type: uint, sampling_rate_ms: int, interpolation_method: uint, ? channel: uint,
  connection_string: tstr, script_path: tstr, ? calibration_table: CborSensorCalibrationDataMap, ? expression: tstr ]
CborSensorConfig = [ id: tstr, metadata: CborSensorMetadataConfig, configuration: CborSensorConfigurationConfig ]
CborSensorsConfig = [ sensors: [*CborSensorConfig] ]
```

`sampling_rate_ms` ≤ 0 means none. `interpolation_method` 0 means none. The stored order is
not significant: sensors are re-sorted into processing order on load. The default configuration
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

sensors_configuration_manager->Update({sensor_1, sensor_5, sensor_6});
```

## Kconfig

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_SENSOR` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION` and `EERIE_LEAP_ADC`; selects `EERIE_LEAP_DOMAIN_SCRIPT` and `EERIE_LEAP_DOMAIN_ADC`. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE` | 8192 | Stack of the processing work queue, where every tick, decode and script call runs. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY` | 6 | Its priority. rt_daq uses 5, above the display and CAN scheduler threads. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS` | n | Periodic rate group statistics log; a measurement aid, off in production. |
| `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS_LOG_PERIOD_S` | 1..3600, 10 | Seconds between statistics lines. |
| `EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT` | 1..65535, 64 | Most sensors a configuration may hold; sizes every snapshot buffer. |
| `EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE` | 1..1024, 32 | Raw CAN frames queued for the log writer; about 90 bytes each, allocated only while a `CANBUS_RAW` sensor exists. |

## Wiring

`SensorsController` (`EERIE_LEAP_CONTROLLER_SENSORS`; depends on `EERIE_LEAP_DOMAIN_SENSOR`,
`EERIE_LEAP_DOMAIN_CANBUS`, `EERIE_LEAP_GPIO`, `EERIE_LEAP_RANDOM`, `EERIE_LEAP_FS`) takes the
internal filesystem, the configuration work queue, the configuration service, the time service,
the shared `SensorReadingsFrame`, the CAN service, the GPIO, and optionally the ADC manager, the
ADC configuration manager and the SD service. It creates the configuration manager (validating
channels against the GPIO and ADC channel counts), the reader factories and the processing
service, registers type 4, and wires the restart handlers. `Start()` starts processing; the
application also registers the processing service with the ADC controller (to be paused during
calibration) and the CAN controller (to be restarted on CAN reconfiguration).

Measured on the ESP32-S3 (application notes): an ADC reading with 40 samples takes about
1.2 ms, 3.3 ms with an expression; a GPIO reading 130 µs, 1.9 ms with an expression.

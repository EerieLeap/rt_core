# Logging

The logging domain (`src/domain/logging_domain/`) writes sensor readings to a file on the SD
card at a fixed interval, in MDF 4.10 by default. The MDF writer itself is the MDF subsystem
(`src/subsys/mdf/`).

## How it works

`LogWriterService` is a pausable service with its own work queue (priority 8, stack
`EERIE_LEAP_LOG_WRITER_STACK_SIZE`). `Start()`:

1. needs a logger, an available SD card and a `logging_interval_ms` of at least 10;
2. creates `EERIE_LEAP_LOG_DATA_FILES_DIR` if missing and picks a new file name
   `<prefix>_<start ms>_<random>.<ext>` (`log_data_1700000000_4242.mf4` by default);
3. opens the file through a seekable stream and calls the logger's `StartLogging()`;
4. schedules its tick every `logging_interval_ms`.

Each tick snapshots every processed reading from the `SensorReadingsFrame` and logs the ones the
configuration enables, then drains the raw CAN frame queue so every frame of a `CANBUS_RAW`
sensor is logged, not only the latest one. The tick allocates nothing: its buffers are sized by
`EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT` and `_RAW_CAN_QUEUE_SIZE`. `Stop()` finalizes and closes
the file.

Starting and stopping is the application's business: rt_daq's `LoggingController` does it from
the CDMP `LOGGING` command (so one broadcast starts every unit, see [cdmp.md](cdmp.md)) and
shows a `log` status on the display. The `logging_events` channel (`StatusUpdated`, payload
`IsActive`) is available for other consumers.

## Loggers

`ILogger<SensorReading>` has `StartLogging(stream, start_time)`, `LogReading(time, reading)`,
`StopLogging()` and the file extension. Two implementations exist:

**`Mdf4LoggerSensorReading`** (used by rt_daq). Built from the current sensors configuration,
so it must be recreated when the sensors change. For every enabled sensor it creates a channel
group named after the sensor ID with a `value` channel (float, unit from the metadata) and, when
`log_raw_value` is set and the sensor is a `PHYSICAL_ANALOG` or `PHYSICAL_INDICATOR` with an
expression, a `raw_value` channel whose MDF conversion is the expression text. `CANBUS_RAW`
sensors get a CAN data frame channel group with variable-length frame data. Each record carries
the time since the file start. The file is written with the "unfinalized" identifier and patched
on `StopLogging()`, so a file cut by a power loss stays readable.

**`SimpleLoggerSensorReading`**. A compact binary format: a header
(`EERIE_LEAP_LOG_DATA_FILE_VERSION`, start time, CRC) followed by one CRC-protected record per
reading (time delta in ms, sensor ID hash, value), plus a separate metadata file
(`EERIE_LEAP_LOG_METADATA_FILE_DIR/_NAME`, `_VERSION`) that describes the sensors.

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
| `max_log_size_mb` | 10 | ≤ 1024. Stored but not enforced yet; files are not rolled over. |
| `sensor_configs[].sensor_id_hash` | | 32-bit hash of the sensor ID (see [smp.md](smp.md)). A sensor without an entry is not logged. |
| `is_enabled` | false | Log this sensor. |
| `log_raw_value` | false | Also log the value before the expression (MDF4: the `raw_value` channel). |
| `log_only_new_data` | false | Skip a tick when the reading's timestamp has not changed since the last record of this sensor. |

The default configuration logs nothing.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_LOGGING` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION`. |
| `EERIE_LEAP_LOG_WRITER_STACK_SIZE` | 8192 | Stack of the writer's work queue (rt_daq uses 3072). |
| `EERIE_LEAP_LOG_DATA_FILES_DIR` | `"data_logs"` | Directory of the log files on the SD card. |
| `EERIE_LEAP_LOG_DATA_FILE_PREFIX` | `"log_data"` | File name prefix; empty drops the prefix and the underscore. |
| `EERIE_LEAP_LOG_DATA_FILE_VERSION` | 0..65535, 0 | Written into the simple logger's data header. |
| `EERIE_LEAP_LOG_METADATA_FILE_DIR` | `"logs_metadata"` | Simple logger metadata directory. |
| `EERIE_LEAP_LOG_METADATA_FILE_NAME` | `"log_metadata"` | Simple logger metadata file. |
| `EERIE_LEAP_LOG_METADATA_FILE_VERSION` | 0..65535, 0 | Its version field. |
| `EERIE_LEAP_MDF` | n | Builds the MDF 4 writer. |

## Wiring

rt_core has no logging controller; an application creates `LoggingConfigurationManager` on
`config/logging_config.cbor` with the configuration work queue, registers it as type 5, creates
`LogWriterService(sd_fs, logging_manager, time_service, readings_frame)`, initializes it, sets a
logger, and starts and stops it as needed. rt_daq's `app/src/controllers/logging_controller.cpp`
is the reference.

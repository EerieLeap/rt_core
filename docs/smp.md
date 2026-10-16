# SMP (MCUmgr) management

Units are managed with the Simple Management Protocol of Zephyr's MCUmgr: CBOR request and
response maps in 8-byte-header packets, carried over CAN between units and over BLE from a phone
or PC. rt_core adds its own groups on top of MCUmgr's `os` group (echo, reset, `mcumgr params`).

Code: `src/subsys/smp/` (group helper, transports), the groups next to their domains
(`src/domain/*/smp/`).

## Groups

Group IDs start at MCUmgr's first user group ID (64). Every group uses SMP v2 group errors
(`err` map with the group and the codes below); for SMP v1 clients they are translated to
`rc` codes. Reads are `read` operations, writes are `write` operations.

| Group | ID | Served by | Commands |
| --- | --- | --- | --- |
| `config` | 64 | `ManagementController` | `LIST` 0, `STORED_CRC` 1, `READ` 2, `WRITE` 3 |
| `device` | 65 | `ManagementController` | `INFO` 0 |
| `network` | 66 | `CanbusController` | `DEVICES` 0 |
| `live` | 67 | `BleService` | `SUBSCRIBE` 0, `UNSUBSCRIBE` 1 |
| `calibration` | 68 | `AdcController` | `START` 0, `STOP` 1, `READ` 2 |
| `files` | 69 | `ManagementController` | `LIST` 0 |

### `config`: read and write stored configurations

Types are those registered with `ConfigurationService` (see [configuration.md](configuration.md)).

| Command | Request | Response |
| --- | --- | --- |
| `LIST` (read) | | `{types: [{type, max}]}`, `max` being the type's size cap |
| `STORED_CRC` (read) | `{type}` | `{type, len, crc}` of the stored file (CRC32) |
| `READ` (read) | `{type, off}` | `{off, data}`; at `off = 0` also `len` and `crc`. The first read takes a snapshot; later offsets read from it. |
| `WRITE` (write) | `{type, off, data}`; at `off = 0` also `len` and `crc`; `tok` on every later chunk | `{off, tok}` with the next offset; `tok` is given on the first chunk |

Rules: one write session and one read snapshot exist at a time; both are freed on completion, on
error, or after `EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS` of inactivity. A client whose answer
to the first write chunk got lost may repeat it unchanged and gets the session's token instead
of `BUSY`. When the last chunk arrives the CRC is checked and the configuration is validated and
stored as received, on the configuration work queue, while the handler waits. A rejected
configuration returns `APPLY_FAILED` with the validator's message in `msg` (up to 191
characters). A configuration that is stored stops logging on every unit (see
[logging.md](logging.md#configuration-updates)); a rejected one does not.

| Error | Code | Meaning |
| --- | --- | --- |
| `UNKNOWN_TYPE` | 2 | No manager registered for that type. |
| `TOO_LARGE` | 3 | `len` above the type's cap, or data beyond `len`. |
| `BAD_OFFSET` | 4 | Not the next offset (the response carries it), or no read snapshot. |
| `BAD_TOKEN` | 5 | Token or type does not match the open session. |
| `BUSY` | 6 | Another client has a write session open. |
| `CRC_MISMATCH` | 7 | |
| `APPLY_FAILED` | 8 | Rejected by validation or could not be stored; see `msg`. |
| `NO_MEMORY` | 9 | The transfer buffer could not be allocated. |

### `device`: identity

`INFO` (read) returns `{family, product, revision, features, hw, sw, build}` (versions packed as
`major << 24 | minor << 16 | patch`), with CDMP `{cdmp_id, uid, device_type, status}`, and on a
unit with sensors `{adc_channels, gpio_channels, sensors_max}`: the ADC and GPIO channels a sensor
can read, each numbered from 0, and the most sensors a configuration holds, which the sensors
configuration is validated against. See [system.md](system.md) and [sensors.md](sensors.md).

### `network`: other units

`DEVICES` (read) with an optional `{off}` returns `{total, off, devices: [{id, uid, type,
status}]}` ordered by CDMP device ID, as many as fit into one response; request the rest from the
next offset. At most `EERIE_LEAP_SMP_NETWORK_MAX_DEVICES` are listed.

### `live`: BLE live values

`SUBSCRIBE` (write) `{ids: [sensor ID hash], period}` replaces the subscription and answers
`{period, max}` with the period actually used (at least 50 ms) and the number of sensors that
fit into one notification. `UNSUBSCRIBE` (write) stops. Errors: `TOO_MANY` (2), `NOT_LISTENING`
(3, the central has not enabled `live` notifications). See [bluetooth.md](bluetooth.md).

### `calibration`: sample one ADC channel

| Command | Request | Response |
| --- | --- | --- |
| `START` (write) | `{channel}` | `{}`; pauses sensor processing and samples the channel until `STOP` |
| `STOP` (write) | | `{}`; resumes processing |
| `READ` (read) | | `{channel, adc_v, sensor_v, ts, seq, adc_err, running}` of the running or last job: volts as float, `ts` in ms since the epoch, `seq` 0 until the first result, `adc_err` the `AdcError` code |

Errors: `BAD_CHANNEL` (2), `BUSY` (3, a job runs or processing could not be paused),
`NOT_RUNNING` (4). See [adc.md](adc.md).

**Sensor ID hashes.** Clients refer to sensors by the 32-bit hash rt_core computes from the ID
string: the low 32 bits of XXH64 of the UTF-8 bytes with seed 0 (`StringHelpers::GetHash`,
`src/utilities/string/xx_hash64.hpp`). The same hash keys the logging configuration.

### `files`: the SD card's files

`LIST` (read) with an optional `{dir, off}` returns `{total, off, entries: [{name, size, type}]}`:
the entries of `dir` from `off`, as many as fit into one response; request the rest from `off`
plus the entries listed, until a page comes back empty or `total` entries are listed. `dir` is
relative to the card's mount point, as a `script_path` is, and the card's root when absent; an
empty request lists the root from its first entry. Entries come in the file system's order, not
sorted, without `.` and `..`. `type` is 0 for a file and 1 for a directory, `size` is in bytes
(0 for a directory), and `total` counts the directory's entries.

Each request reads the directory again from its start, at most 48 entries, on the configuration
work queue while the handler waits: paging keeps no state on the unit and holds together while
the directory does not change. `dir` is at most 128 bytes. Names are the card's FAT long names in
its code page (437 by default), not UTF-8, so only ASCII names work in a `script_path`. A card is
mounted when the unit starts; one put in later is mounted only while the SD monitor runs (see
[filesystem.md](filesystem.md)). A unit without an SD service still serves the group and answers
`NO_CARD`; firmware without the group answers `rc` 8 (`MGMT_ERR_ENOTSUP`).

| Error | Code | Meaning |
| --- | --- | --- |
| `NO_CARD` | 2 | The unit has no SD service, or no card is mounted. |
| `NOT_FOUND` | 3 | No such directory. |
| `BAD_PATH` | 4 | `dir` is absolute, has a `..` segment, a backslash or a NUL, is longer than 128 bytes, or names a file. |
| `IO_ERROR` | 5 | The card could not be read. |
| `NO_MEMORY` | 6 | The entries could not be stored. |

## Transports

| Transport | Kconfig | Route |
| --- | --- | --- |
| CAN | `EERIE_LEAP_SMP_CAN` | Between units on the CDMP COM channel; see [cdmp.md](cdmp.md). |
| BLE | `EERIE_LEAP_SMP_BLE` | From a central to this unit or, forwarded over CAN, to another unit; see [bluetooth.md](bluetooth.md). |

Both transports share MCUmgr's packet pool, so a client keeps no more requests in flight than
`mcumgr params` reports.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_SMP` | n | MCUmgr server with the `os` group. Selects `MCUMGR`, `MCUMGR_GRP_OS`, `MCUMGR_GRP_OS_MCUMGR_PARAMS`, `ZCBOR`, `NET_BUF`, `CRC`, `EERIE_LEAP_RANDOM`, `EERIE_LEAP_THREADING`. The domain groups are only compiled with it. |
| `EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS` | 5000 | Idle time after which a config transfer is abandoned. |
| `EERIE_LEAP_SMP_NETWORK_MAX_DEVICES` | 1..254, 32 | Devices the `network` group can list. |
| `MCUMGR_TRANSPORT_NETBUF_COUNT` | 6 | Packet pool size (rt_core default; boards may override). |
| `MCUMGR_TRANSPORT_NETBUF_SIZE` | 1024 | Largest packet. |
| `MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO` | 7 | MCUmgr handler thread, below sensor processing. |
| `MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE` | 4096 with `EERIE_LEAP_CONTROLLER_ADC` | The calibration group pauses and resumes processing on this queue, which 2048 bytes do not cover. |
| `EERIE_LEAP_CONTROLLER_MANAGEMENT` | n | Serves `config`, `device` and `files`. Depends on `EERIE_LEAP_SMP` and `EERIE_LEAP_DOMAIN_SYSTEM`; create it after every configuration manager is registered. |

## Writing a group

Derive from `SmpGroup<YourGroup>` with the group ID, a static `mgmt_handler` table whose entries
call `Dispatch<&YourGroup::HandleX>`, and an error translation function; call `Register()` once
the group is ready. Handlers run on the MCUmgr work queue with a small stack: decode with
`SmpDecodeRequest()` and `SmpMapKey()`, answer group errors with `SmpAddGroupError()`, and
move heavy work to another queue as `ConfigMgmtGroup` does. One instance per group type can be
registered.

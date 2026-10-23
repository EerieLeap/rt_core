# SMP (MCUmgr) management

Units are managed with the Simple Management Protocol of Zephyr's MCUmgr: CBOR request and
response maps in 8-byte-header packets, carried over CAN between units and over BLE from a phone
or PC. rt_core adds its own groups on top of MCUmgr's `os` group (echo, reset, `mcumgr params`).

Code: `src/subsys/smp/` (group helper, transports), the groups next to their domains
(`src/domain/*/smp/`), the `assets` group with the assets (`src/subsys/assets/smp/`), the `dfu` group
with the firmware update (`src/subsys/dfu/smp/`).

## Groups

Group IDs start at MCUmgr's first user group ID (64). Every group uses SMP v2 group errors
(`err` map with the group and the codes below); for SMP v1 clients they are translated to
`rc` codes. Reads are `read` operations, writes are `write` operations.

Two groups are MCUmgr's own: `os` (0: echo, `mcumgr params`, reset) is on every unit, and `img_mgmt`
(1: image list, upload, state, erase) is on every unit with `EERIE_LEAP_DFU`. They need no group of
rt_core, work unchanged over every transport, and pass the BLE access point's router like any other
packet (it parses the 8-byte header only). See [dfu.md](dfu.md).

| Group | ID | Served by | Commands |
| --- | --- | --- | --- |
| `os` | 0 | MCUmgr (`EERIE_LEAP_SMP`) | `ECHO` 0, `MCUMGR_PARAMS` 6, `RESET` 5, `BOOTLOADER_INFO` 8 (with `EERIE_LEAP_DFU` under MCUboot) |
| `img_mgmt` | 1 | MCUmgr (`EERIE_LEAP_DFU`) | `STATE` 0 (read: list; write: test, confirm), `UPLOAD` 1, `ERASE` 5 |
| `config` | 64 | `ManagementController` | `LIST` 0, `STORED_CRC` 1, `READ` 2, `WRITE` 3 |
| `device` | 65 | `ManagementController` | `INFO` 0 |
| `network` | 66 | `CanbusController` | `DEVICES` 0 |
| `live` | 67 | `BleService` | `SUBSCRIBE` 0, `UNSUBSCRIBE` 1 |
| `calibration` | 68 | `AdcController` | `START` 0, `STOP` 1, `READ` 2 |
| `files` | 69 | `ManagementController` | `LIST` 0 |
| `assets` | 70 | `ManagementController` | `KINDS` 0, `LIST` 1, `READ` 2, `WRITE` 3, `DELETE` 4, `STAT` 5, `RENAME` 6 |
| `logging` | 71 | `LoggingController` | `STATUS` 0, `START` 1, `STOP` 2 |
| `dfu` | 72 | `DfuController` | `STATUS` 0 |
| `ui` | 73 | served by the application (rt_gauge's UI preview) | `BEGIN` 0, `STAGE` 1, `PATCH` 2, `SHOW` 3, `HIGHLIGHT` 4, `END` 5, `STATUS` 6 (read) |

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

`INFO` (read) returns `{product, revision, features, hw, sw, build, board}` (versions packed as
`major << 24 | minor << 16 | patch`; `build` is the application `VERSION` file's tweak; `board` is the text
`CONFIG_BOARD_TARGET`, which with `product` tells which image the unit accepts; `features` is the
`ProductFeature` mask the unit sends on CDMP now, the build's without CDMP), with CDMP `{cdmp_id, uid, status}`, and on a
unit with sensors `{adc_channels, gpio_channels, sensors_max}`: the ADC and GPIO channels a sensor
can read, each numbered from 0, and the most sensors a configuration holds, which the sensors
configuration is validated against. Always present: `event_channels: [uint]`, the `EventChannelId` of
each channel the unit registered (empty without the event bus), and `settings: [tstr]`, the ids of the
settings it carries (the application passes them to `ManagementController`; none by default). See [system.md](system.md) and [sensors.md](sensors.md).

### `network`: other units

`DEVICES` (read) with an optional `{off}` returns `{total, off, devices: [{id, uid, features,
status}]}` ordered by CDMP device ID, as many as fit into one response; request the rest from the
next offset. At most `EERIE_LEAP_SMP_NETWORK_MAX_DEVICES` are listed.

### `live`: BLE live values

`SUBSCRIBE` (write) `{ids: [sensor ID hash], period, fields?: [uint]}` replaces the subscription and
answers `{period, max, fields: true}` with the period actually used (at least 50 ms) and the number of
entries that fit into one notification. `fields` is parallel to `ids`: 0 value, 1 calibrated voltage,
2 ADC voltage; absent means 0 for every entry. A different length or an unknown field is `EINVAL`. A
sensor may be listed several times with different fields. Older firmware ignores `fields` and answers
without `fields: true`. `UNSUBSCRIBE` (write) stops. Errors: `TOO_MANY` (2), `NOT_LISTENING`
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
[filesystem.md](filesystem.md)). A unit without an SD service, which reports no `STORAGE`
feature, still serves the group and answers `NO_CARD`; firmware without the group answers `rc` 8
(`MGMT_ERR_ENOTSUP`).

| Error | Code | Meaning |
| --- | --- | --- |
| `NO_CARD` | 2 | The unit has no SD service, or no card is mounted. |
| `NOT_FOUND` | 3 | No such directory. |
| `BAD_PATH` | 4 | `dir` is absolute, has a `..` segment, a backslash or a NUL, is longer than 128 bytes, or names a file. |
| `IO_ERROR` | 5 | The card could not be read. |
| `NO_MEMORY` | 6 | The entries could not be stored. |

The group only lists. Scripts are put on the card, and logs fetched from it, with the
[`assets`](#assets-asset-kinds) group.

### `assets`: asset kinds

Files a unit keeps for its own use, such as images, animations, Lua scripts and logs, grouped by
kind. A client addresses a file by its kind and a flat name, never by a path: the unit decides the
directory, the extensions, what a client may do with the kind and how large an upload can be. Each
controller registers the kinds it owns with the `AssetsManager` ([filesystem.md](filesystem.md#assets-eerie_leap_assets)),
so a unit serves the kinds it has the services for, and `KINDS` says which.

| Command | Request | Response |
| --- | --- | --- |
| `KINDS` (read) | | `{kinds: [{kind, name, dir, access, max, ext, avail, free?, used?}]}` ordered by `kind`: the number, the name (`"images"`), the directory on the volume, the allowed operations as bits (1 download, 2 upload, 4 delete), the largest upload in bytes (0: none), the extensions with their dot (`[".bin"]`) whether the volume is mounted now and, only while it is, `free` and `used` (bytes on the kind's volume). `free` is the number `WRITE` checks `len` against, and only a hint: files take whole blocks, so a `len` below it can still end in `NO_SPACE`. A unit without kinds answers an empty list. |
| `LIST` (read) | `{kind, off?}` | `{entries: [{name, size, end_ms?}], next?}` |
| `READ` (read) | `{kind, name, off}` | `{off, data}`; at `off = 0` also `len`, the size of the file; the chunk that ends at `len` also `crc`, the CRC32 of the whole file |
| `WRITE` (write) | `{kind, name, off, data}`; at `off = 0` also `len` and `crc`, the size and the CRC32 of the whole file; `tok` on every later chunk | `{off, tok}` with the next offset, `len` once the file is stored; `tok` is given on the first chunk |
| `DELETE` (write) | `{kind, name}` | `{}` |
| `STAT` (read) | `{kind, name}` | `{len, crc?}`: the size of the file and the CRC32 of the whole file, the one `READ` sends; `crc` is left out for a file above `EERIE_LEAP_SMP_ASSETS_STAT_CRC_MAX_SIZE` that no current checkpoint covers |
| `RENAME` (write) | `{kind, name, to}` | `{}` |

The CRC32 is the IEEE one (`zlib.crc32`). The kind numbers are `0` `images`, `1` `animations`,
`2` `sensor_scripts`, `3` `can_scripts` and `4` `logs`; they are append only, and
[filesystem.md](filesystem.md#assets-eerie_leap_assets) lists which unit registers which, with
directory, access and size cap. A name is 1 to `EERIE_LEAP_ASSETS_MAX_NAME_LENGTH` (64) bytes of
`A-Z a-z 0-9 . _ -`, not starting with a dot, ending in one of the kind's extensions (compared
ignoring ASCII case) with at least one character before it; any other name is `BAD_NAME`. `READ`,
`DELETE` and the first chunk of `WRITE` check, before anything else and in this order, the kind
(`UNKNOWN_KIND`), its `access` (`DENIED`), the name (`BAD_NAME`) and the volume (`UNAVAILABLE`). `LIST`
checks the kind and the volume only: it needs no access bit and takes no name. A later chunk of
`WRITE` is matched to the open upload instead, and checks neither access nor name (see below).

**`LIST`.** `end_ms` is the end of the file in milliseconds since the Unix epoch (UTC), given for the
`logs` kind only and only for a file this unit's writer recorded when it finalized it (a normal stop
or a rotation; not a file left unfinalized, a log from before the index, or one copied onto the card);
the duration is `end_ms` less the start in the name.

The entries come from directory position `off` (0 when absent), at most 48 directory
entries per request, in the file system's order, not sorted. Only files whose names are valid for the
kind are listed: directories, dot files and other extensions are skipped but counted. `next` is the
position to ask for next, absent once the directory is done; a page can be empty while `next` is
present, so a client asks until `next` is absent. A directory that does not exist yet lists nothing.
Paging keeps no state on the unit and holds together while the directory does not change.

**`READ`.** A download is one session on one file: the unit keeps the file open and a running CRC32
of the bytes up to the farthest it has read, and the chunk that reaches `len` carries it as `crc`. A
request at `off = len` answers empty data and `crc`; `off` past `len` is `BAD_OFFSET`, and the
answer carries `len`. Every chunk but the last has the same length whatever its offset, a little
under `MCUMGR_TRANSPORT_NETBUF_SIZE`, so a client learns it from the first answer and may keep
several requests in flight at multiples of it (see [Transports](#transports)). `IN_USE` is asked
when a session opens.

- A request at `off = 0` or for another file replaces the session; a request at `off > 0` for the
  open file continues it.
- A request past the bytes the CRC covers (a request lost in a pipeline, a resume) makes the unit read
  the gap first, so it takes as long as that read. A chunk below the CRC (a repeat after a lost
  answer) leaves it alone.
- The session closes after the last chunk, on a failure to read the file or to extend the CRC
  (`IO_ERROR`), when a request replaces it (also when that request then fails, `NOT_FOUND` or `IN_USE`),
  when the file is deleted or replaced, when a `READ` of its kind finds the volume unavailable and
  after `EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS` without a request. A refused request (`BAD_OFFSET`,
  `UNKNOWN_KIND`, `DENIED`, `BAD_NAME`) leaves it open.
- A closing session leaves a checkpoint of its CRC, for one file and its size, the last one closed: a
  client that resumes at `off > 0` on that file continues without the unit reading it again. After a
  reboot there is none, and the unit reads the file up to `off` before it answers. A download from
  `off = 0` never uses the checkpoint, as the file may have changed, and a delete or an upload of the
  file drops it.
- An upload that replaces the file meanwhile closes the session: the next request opens the new
  file, and the `crc` it ends with is the new file's, so a client that checks its data against it
  sees the mix.

**`WRITE`.** One upload is open at a time, for all kinds, held by the client that has its token. The
first chunk carries `len` and `crc` and is answered with the token `tok`; every later chunk carries
the token and starts where the last one ended. A file of no bytes is a first chunk with `len` 0 and
empty data. The data goes to a staged file, `.assets_tmp/upload.part` at the root of the kind's
volume, which no kind lists (the `files` group lists directories as they are, so it shows
`.assets_tmp/` at the root of the card, and `upload.part` in it while an upload is staged). With the
last chunk the unit checks the CRC32 of everything it received and renames the staged file onto the
asset, replacing it (the kind's directory is created when missing). The first chunk is checked after the kind, access, name and volume, in this order:
the size (`TOO_LARGE`: `len` above `max`, or more data than `len`), whether the file is in use
(`IN_USE`), whether an upload is open (`BUSY`) and the room (`NO_SPACE`: `len` above the free space
of the volume, checked once; the old file counts as used until it is replaced, so replacing a file
needs room for both).

- A later chunk is matched to the open upload by its token, kind and exact name (`BAD_TOKEN` when
  there is none or they differ), then checked for the kind (`UNKNOWN_KIND`), the volume
  (`UNAVAILABLE`), the offset (`BAD_OFFSET`) and the length (`TOO_LARGE`), in this order.
- An upload ends, with its staged file deleted and the asset as it was, on `CRC_MISMATCH`, on data
  beyond `len` (`TOO_LARGE`), on a failure of the file system (`NO_SPACE` for a full volume,
  `IO_ERROR`), when the file went into use (`IN_USE`, asked again just before the file is replaced), when
  the kind or its volume went away (`UNKNOWN_KIND`, `UNAVAILABLE` on a later chunk) and after
  `EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS` without a request, which makes it no upload for the next
  request, whoever sends it. `BAD_OFFSET`, `BAD_TOKEN` and `BUSY` answer one request and leave the
  upload as it was.
- A staged file that a power loss or a pulled card left is deleted when the group starts and before
  the next upload checks the room.
- A client whose answer got lost sends the request again. The first chunk, unchanged, while nothing came
  after it, is answered with the upload's `{off, tok}` instead of `BUSY`. A chunk the unit already
  holds is answered `BAD_OFFSET` with `off`, the offset it expects. The last chunk, within the
  session timeout after the file was stored, is answered `{off: len}` again and not written; later it is
  `BAD_TOKEN`, and the client reads the outcome with `LIST` or `READ`. A file sent in one chunk that
  was stored, sent again without a token within the timeout and while the file is still as stored,
  is answered `{off: len, tok}` again.
- A first chunk with the upload's own token starts the upload over, for the same file or another: the
  staged data goes, and the room it held counts for the check.

**`STAT`.** Lets a client compare a file with a local one without downloading it. Checked as `READ` is,
in this order: the kind (`UNKNOWN_KIND`), its read access (`DENIED`), the name (`BAD_NAME`), the volume
(`UNAVAILABLE`), the file (`NOT_FOUND`) and whether it is in use (`IN_USE`). The CRC comes from the
running CRC of an open download that has read the whole file, or from the checkpoint when it is
current ([filesystem.md](filesystem.md#assets-eerie_leap_assets)); otherwise the unit reads the file
on the configuration work queue while the client waits, up to `EERIE_LEAP_SMP_ASSETS_STAT_CRC_MAX_SIZE`,
and keeps the result as the checkpoint. A larger file answers its size alone. An open download is left
as it is.

**`RENAME`.** Moves a file within its kind and replaces `to` if it exists, as a `WRITE` would. It
takes a name away and makes one, so the kind needs both the upload and the delete bit. Checked in this
order: the kind (`UNKNOWN_KIND`), its access (`DENIED`), both names (`BAD_NAME`), two names that differ
only in ASCII case (`BAD_NAME`: FAT would delete the file; a case change needs a copy and a delete),
the volume (`UNAVAILABLE`), whether `name` exists (`NOT_FOUND`), whether the kind uses `name` (as for
`DELETE`) or `to` (as for `WRITE`) (`IN_USE`), and whether an upload of either name is open (`BUSY`). A
download of either name is closed first.

**`DELETE`.** A kind may refuse (`IN_USE`): a script that a stored configuration refers to is kept,
and so is the log the writer is writing, which cannot be downloaded either. A download of the file is
closed first.

**File system.** Every command runs on the configuration work queue while the handler waits, as
`config` and `files` do, so the MCUmgr stack stays small and the group needs no lock. Its file I/O
runs next to the log writer's on the same SD card, which needs FatFs reentrancy. FatFs has no
open-file locking: deleting or replacing an open file corrupts the volume, so the group closes its own
download of a file before it deletes the file or stores an upload over it. A rename onto an
existing file is atomic on littlefs; on FAT it is a delete and a rename, and a power loss between
them loses the asset (the staged copy goes at the next start). A group that cannot get its transfer
buffer (`MCUMGR_TRANSPORT_NETBUF_SIZE` bytes of external RAM) is left out and the unit answers
`rc` 8; so does firmware without the group.

| Error | Code | Meaning |
| --- | --- | --- |
| `UNKNOWN` | 1 | An unexpected failure on the unit; logged there. |
| `UNKNOWN_KIND` | 2 | No kind with that number is registered. |
| `UNAVAILABLE` | 3 | The kind's volume is not mounted. |
| `NOT_FOUND` | 4 | No such asset. |
| `BAD_NAME` | 5 | The name is not valid for the kind. |
| `DENIED` | 6 | The kind does not allow the operation (its `access` bits). |
| `IN_USE` | 7 | The kind refuses the operation on that file now. |
| `TOO_LARGE` | 8 | `len` above the kind's `max`, or data beyond `len`. |
| `NO_SPACE` | 9 | The volume has no room for `len`, or filled up during the upload. |
| `BAD_OFFSET` | 10 | `READ`: `off` past the end; the response carries `len`. `WRITE`: not the next offset; the response carries the one the unit expects as `off`. |
| `BAD_TOKEN` | 11 | No upload is open, or the token, kind or name does not match it. |
| `BUSY` | 12 | Another client has an upload open. |
| `CRC_MISMATCH` | 13 | The received data does not match the `crc` of the first chunk. |
| `IO_ERROR` | 14 | The file system failed to read, write, delete or rename. |
| `NO_MEMORY` | 15 | No memory, or no free file slot. |

SMP v1 clients get `rc` instead: `ENOENT` for `UNKNOWN_KIND` and `NOT_FOUND`, `EBADSTATE` for
`UNAVAILABLE`, `EINVAL` for `BAD_NAME`, `BAD_OFFSET`, `BAD_TOKEN` and `TOO_LARGE`, `EACCESSDENIED`
for `DENIED`, `EBUSY` for `IN_USE` and `BUSY`, `ENOMEM` for `NO_SPACE` and `NO_MEMORY`, `ECORRUPT`
for `CRC_MISMATCH` and `EUNKNOWN` for the rest.

### `logging`: status, start and stop

The CDMP `LOGGING` command and the shared status reach only units on the CAN bus. This group gives a
client that talks SMP, over Bluetooth or routed through an access point, the same view and the same
controls. Both roles serve it (see [logging.md](logging.md#control)); `LoggingController` registers it
when `EERIE_LEAP_SMP` is on.

| Command | Request | Response |
| --- | --- | --- |
| `STATUS` (read) | | `{active, reason, since_ms?, file?, bytes?}` |
| `START` (write) | `{uid?}` | `{}` |
| `STOP` (write) | `{uid?}` | `{}` |

- `active` is a boolean, `reason` the `LoggingStatusReason` (0 `Stopped`, 1 `WriteFailed`, 2 `NoSpace`,
  3 `RotationFailed`, 4 `Unavailable`, 5 `FirmwareUpdate`) of the last status on `logging_events`; 0
  while `active`, as in the CDMP record. On a unit with a log writer it is that unit's own status (after
  a firmware upload started, `FirmwareUpdate`). On a unit without one (the gauge) it is the combined
  status the units share, and `Unavailable` while none reports.
- `since_ms` is the start of the session in Unix milliseconds, UTC, and only present while this unit's
  own writer runs a session. `file` (the asset name in the [`logs`](#assets-asset-kinds) kind) and
  `bytes` (that file's size so far, headers included) are present only while a file is open; the file
  being written cannot be read as an asset. A unit without a writer never sends the three.
- `uid` is the CDMP UID of the unit to start or stop; absent or 0 is every unit that can log, as in the
  `LOGGING` command. The request is the control's `RequestStart()` or `RequestStop()`, the same as a
  trigger makes: the answer `{}` means the request was taken, not that logging changed, and a start
  that fails or a stop while stopped publishes nothing. A client reads `STATUS` again to see the effect.
  A unit with a writer acts on 0 and its own UID itself and sends the rest to the network; the gauge
  sends the `LOGGING` command.

Errors: the group has no error codes of its own. A `uid` that is not an unsigned integer that fits 32
bits is a bad request (`rc` `EINVAL`).

Threading: the handlers run on the MCUmgr work queue and only read atomics, call the writer's session
getter and store a request; they allocate, block and throw nothing.

### `dfu`: firmware update status and errors

The standard `img_mgmt` group does the transfer; this one reports what it does not
(see [dfu.md](dfu.md) for the whole update and its confirmation).

| Command | Request | Response |
| --- | --- | --- |
| `STATUS` (read) | | `{confirmed, state, confirm_in_ms?, reset_cause}` |

- `confirmed` is a boolean: the running image is confirmed. `state` is 0 `CONFIRMED`, 1 `VERIFYING` (a
  test boot waiting for boot complete), 2 `CONFIRMING` (boot complete, counting the self-confirmation
  delay) or 3 `FAILED` (the deadline passed, a reset follows). `confirm_in_ms` is the time left in
  `CONFIRMING` only. `reset_cause` is the `hwinfo` mask of this boot.
- Errors, in the `err` map of the answer to an `img_mgmt` upload (the identity check refuses the
  chunk): `NO_IDENTITY` 2 (no identity TLV in the image), `WRONG_PRODUCT` 3, `WRONG_BOARD` 4,
  `BAD_IDENTITY` 5 (malformed header, TLVs or identity). For SMP v1 clients they translate to `EINVAL`.
  The refused upload's partial image is erased, so it is not listed. The answer to the refused chunk
  carries `err` and also `off: 0` (`img_mgmt` appends its offset after the hook refuses and the upload
  state has been reset): check `err` before `off`. A refusal emits no `DFU_STOPPED` notification.

Threading: the handler reads a snapshot from the confirmation state and allocates, blocks and throws
nothing.

### `ui`: preview a UI configuration

Group 73 is reserved here (`SmpGroupId::UI`) and served by the application: rt_gauge's UI preview
(`EERIE_LEAP_UI_PREVIEW`). A client stages a whole UI configuration in chunks, which the unit checks as an
upload is (`CborConfigurationManagerBase::ParseCborConfiguration()`) and shows without storing it, so a preview
stores nothing and stops no logging (see [logging.md](logging.md#configuration-updates)). Its commands are
`BEGIN` 0, `STAGE` 1, `PATCH` 2, `SHOW` 3, `HIGHLIGHT` 4, `END` 5 and `STATUS` 6 (read); the schema is rt_gauge's
`ui_preview_smp.cddl`. A unit built without it answers the group as it answers any group it does not serve.

## Transports

| Transport | Kconfig | Route |
| --- | --- | --- |
| CAN | `EERIE_LEAP_SMP_CAN` | Between units on the CDMP COM channel; see [cdmp.md](cdmp.md). |
| BLE | `EERIE_LEAP_SMP_BLE` | From a central to this unit or, forwarded over CAN, to another unit; see [bluetooth.md](bluetooth.md). |

Firmware images are `img_mgmt` upload chunks of `buf_size - 64` bytes, except the first, which also
carries `image`, `len` and the 32-byte `sha` and has to be sized by encoding it (`buf_size - 64`
overflows the 1024-byte packet); a first chunk with a full `sha` waits for the unit's check of the slot
like the last one (allow about 5 s). **Window rule for image chunks
over CAN:** a client sends them to a unit behind CAN **one at a time** (a window of 1). The target
erases a flash sector for every 4 KB of image with interrupts off for tens of milliseconds, during
which the CAN controller's receive buffers fill and frames are lost; a second request already on the
wire arrives during an erase and is dropped, and the client then waits out its timeout. One chunk in
flight costs about a tenth of the rate and loses nothing. Over BLE to the access point itself, the
`info` window stays. The AP forwards image chunks to a peer as it forwards any packet (pass-through,
no staging), so the phone stays connected for the whole transfer.

Both transports share MCUmgr's packet pool, and requests and responses draw from it, so a client
keeps fewer requests in flight than the `buf_count` that `mcumgr params` reports: a window of
`buf_count` exhausts the pool. No packet exceeds `buf_size`, which the chunks of `config` and
`assets` are sized from. BLE reports its window in the `info` characteristic
(`EERIE_LEAP_SMP_BLE_REQUESTS_IN_FLIGHT`, 2 by default). Over CAN the rate of a transfer is the
sending unit's `smp_bus_share_percent` (25 % by default, see [canbus.md](canbus.md)) of the bus.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_SMP` | n | MCUmgr server with the `os` group. Selects `MCUMGR`, `MCUMGR_GRP_OS`, `MCUMGR_GRP_OS_MCUMGR_PARAMS`, `ZCBOR`, `NET_BUF`, `CRC`, `EERIE_LEAP_RANDOM`, `EERIE_LEAP_THREADING`. The domain groups are only compiled with it. |
| `EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS` | 5000 | Idle time after which a config transfer is abandoned. |
| `EERIE_LEAP_SMP_NETWORK_MAX_DEVICES` | 1..254, 32 | Devices the `network` group can list. |
| `EERIE_LEAP_SMP_ASSETS` | y | Serves the `assets` group. Depends on `EERIE_LEAP_ASSETS` and `EERIE_LEAP_CONTROLLER_MANAGEMENT`, and on `FS_FATFS_REENTRANT` when the file system is FAT (`FAT_FILESYSTEM_ELM`), which Zephyr leaves off. Selects `MCUMGR_SMP_CBOR_MIN_ENCODING_LEVEL_4` with `ZCBOR_CANONICAL`: `KINDS` nests four levels. |
| `EERIE_LEAP_SMP_ASSETS_STAT_CRC_MAX_SIZE` | 4194304 | Largest file `STAT` reads to compute a CRC; a larger one answers its size alone unless a checkpoint covers it. |
| `EERIE_LEAP_SMP_ASSETS_SESSION_TIMEOUT_MS` | 5000 | Idle time after which a download is closed or an upload abandoned and its staged file deleted. |
| `MCUMGR_TRANSPORT_NETBUF_COUNT` | 6 | Packet pool size (rt_core default; boards may override). |
| `MCUMGR_TRANSPORT_NETBUF_SIZE` | 1024 | Largest packet. The `assets` group's transfer buffer has this size, and a `READ` chunk is a little smaller. |
| `MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO` | 7 | MCUmgr handler thread, below sensor processing. |
| `MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE` | 4096 with `EERIE_LEAP_CONTROLLER_ADC` or `EERIE_LEAP_DFU` | The calibration group pauses and resumes processing on this queue, which 2048 bytes do not cover; `img_mgmt` (bootutil trailer reads, stream flash writes, the image check) runs on it too. |
| `EERIE_LEAP_DFU` and its options | y with `BOOTLOADER_MCUBOOT` and `EERIE_LEAP_DOMAIN_SYSTEM` | `img_mgmt`, the identity check, the `dfu` group and the confirmation; see [dfu.md](dfu.md#kconfig). |
| `FS_LITTLEFS_NUM_FILES`, `FS_FATFS_NUM_FILES` | 6 with `EERIE_LEAP_SMP_ASSETS` | Open files of the file system the build uses (littlefs, FAT): a download and an upload stay open between requests, next to the log writer and the transient opens. rt_core's default. |
| `EERIE_LEAP_CONTROLLER_MANAGEMENT` | n | Serves `config`, `device`, `files` and, with `EERIE_LEAP_SMP_ASSETS`, `assets`. Depends on `EERIE_LEAP_SMP` and `EERIE_LEAP_DOMAIN_SYSTEM`; create it after every configuration manager is registered. It serves the `AssetsManager` it is given, whose kinds may come and go; given none, `KINDS` lists nothing. |

## Writing a group

Derive from `SmpGroup<YourGroup>` with the group ID, a static `mgmt_handler` table whose entries
call `Dispatch<&YourGroup::HandleX>`, and an error translation function; call `Register()` once
the group is ready. Handlers run on the MCUmgr work queue with a small stack: decode with
`SmpDecodeRequest()` and `SmpMapKey()`, answer group errors with `SmpAddGroupError()`, and
move heavy work to another queue as `ConfigMgmtGroup` does. One instance per group type can be
registered.

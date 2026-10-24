# Filesystem and assets

Code: `src/subsys/fs/`, `src/subsys/assets/`.

## Filesystem service (`EERIE_LEAP_FS`)

`FsService` wraps one Zephyr `fs_mount_t` taken from the device tree `fstab` (see
[device-tree.md](device-tree.md)): the entry itself, not a copy, because `fs_mount()` links it
into the global mount list. Every path is relative to the mount point and limited to 256 bytes
with the mount prefix. The interface (`IFsService`) offers `WriteFile` (overwrite or append),
`ReadFile`, `OpenFile`, `CreateDirectory`, `Exists`, `DeleteFile`, `DeleteRecursive`, `Rename`
(replaces a file at the target: atomic on littlefs, a delete and a rename on FAT; a missing source
fails with `-ENOENT` before the target is touched), `ListFiles`, `ListDirectory` (a page of a
directory's entries with their sizes and types), `GetFileSize` (of a file: a directory has none),
`GetTotalSpace`, `GetUsedSpace` (both in bytes of the volume's allocation units, a FAT cluster or a
littlefs block, so the difference is the room that files can still take, as a hint: every file takes whole blocks, so a file below the free space can still fail with `-ENOSPC`, and the write stays the authority) and `Format`. All calls
hold one mutex per service.

Zephyr's FAT rename deletes the target first, so a file renamed onto itself is lost. `Rename`
therefore takes only plain paths, `A-Z a-z 0-9 . _ -` and `/` with no name ending in a dot, and
refuses a pair that names one file ignoring ASCII case, both with `-EINVAL`: within that alphabet
FAT and littlefs resolve a name as written, while FatFs folds other spellings onto one name (a
backslash is a separator, trailing dots and spaces are dropped, a control byte ends the path).

Two instances exist in the application:

| Instance | Mount | Holds |
| --- | --- | --- |
| Internal (`FsService`) | alias `intfs0`, littlefs on internal flash | `config/*.cbor`, `ui_assets/` (rt_gauge's images, animations and fonts) |
| SD card (`SdmmcService`) | alias `sdfs0` on the `zephyr,sdhc` disk, FAT | Lua scripts (`scripts/sensors`, `scripts/can`), `logs/` |

The SD service is optional for the sensors and CAN code, which accept a null SD service and then
skip script validation and serve no script kind. The writer-role `LoggingController` requires one, and its `Initialize()`
fails without it, so a composition root with no SD service does not build it (rt_daq skips it). The
remote role ignores the SD service. See [logging.md](logging.md#wiring).

Clients list a directory of the card, such as `scripts/`, with the SMP `files` group, and list,
download, upload and delete the files of the asset kinds below with the `assets` group. The
`ManagementController` serves both, `files` with the SD service and `assets` with the registry it is
given (see [smp.md](smp.md)).

## Stream buffer

`FsServiceStreamBuf` is a seekable `std::streambuf` over a file of a service, in read, write or
append mode. Reads go through a 4 KiB buffer. Writes collect in a put area of
`EERIE_LEAP_FS_STREAM_WRITE_BUFFER_SIZE` bytes that is written in one `fs_write()` when it fills,
on a seek, a read, `pubsync()` and `close()`; a request of at least the buffer size goes to the
file directly. A write that fails or falls short latches the stream: every later write is refused,
so a caller sees one failure instead of a file with a hole in it. `GetBytesWritten()` counts the
bytes accepted and `GetWriteCallCount()` the `fs_write()` calls. The MDF4 logger writes through
it; its sync cadence is the logging domain's (`EERIE_LEAP_LOG_SYNC_INTERVAL_MS`, see
[logging.md](logging.md)).

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_FS_STREAM_WRITE_BUFFER_SIZE` | 0..65536, 4096 | Put area of write and append streams; 0 writes every request straight to the file. Fewer, larger writes, not a larger loss window: the writer's sync interval bounds that. |

## SD card service

`SdmmcService` extends `FsService` for a removable card:

- `IsAvailable()` is true while the card is present and mounted. Presence comes from the
  registered handler (`DtFs::IsSdCardPresent`): native card detection on SDHC hosts, a status
  command on SPI hosts, which cannot detect the card otherwise.
- `SdMonitorStart()`/`SdMonitorStop()` run a thread (priority 2) that polls presence every
  `EERIE_LEAP_SD_CHECK_INTERVAL_MS` and mounts or unmounts accordingly. The application does
  not start it on SPI hosts, where the status command interferes with logging.

The service is built with `CONFIG_DISK_ACCESS`, and then `EERIE_LEAP_FS` selects
`EERIE_LEAP_THREADING` for the monitor thread.

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_FS_SD_THREAD_STACK_SIZE` | 3072 | Stack of the monitor thread. |
| `EERIE_LEAP_SD_CHECK_INTERVAL_MS` | 1000 | Presence polling interval. |

## Assets (`EERIE_LEAP_ASSETS`)

Code: `src/subsys/assets/`; selects `EERIE_LEAP_FS`. An asset is a file of one kind (images,
animations, fonts, Lua scripts, logs) that a client lists, downloads, uploads and deletes over SMP by kind
and name, without knowing the directory behind it (the `assets` group, see [smp.md](smp.md)).

**`AssetStore(fs_service, directory)`** is the files of one directory of one file system service
(`directory` is relative to the mount point; empty is the volume's root). The directory is created
when the store is, if the volume is available, otherwise when a write needs it.

- Whole files by a path relative to the directory: `Save`, `Load` (into a PMR vector on the external
  heap; empty for a file that is missing, empty, unreadable or that changed size while it was read),
  `Delete`, `Exists` and `GetSize` (nothing for a directory). No name rules apply here: a widget's
  `FILE_PATH` may hold a sub-path.
- `IsAvailable()`, `GetSpace()` (free and used bytes from one reading of the volume's total and used space; both 0 when unavailable) and `GetFreeSpace()` (its `free`). Free space is a hint, as above; the SMP `assets` `KINDS` reports both.
- `List(first, max_entries, extensions, ...)` reads a page of the directory and keeps the files whose
  names are valid for the extensions; directories and other names are skipped but counted, and each
  kept entry carries its directory position. A directory that does not exist lists nothing.
- `OpenRead(name, reader)` and `BeginWrite(name, writer)` stream a file (below).

**`AssetReader` and `AssetWriter`** stream a file through a buffer of `BUFFER_SIZE` (4 KiB) bytes on
the external heap, aligned to 32 bytes for block devices that transfer by DMA. A reader fills the
buffer at multiples of 4 KiB, so the file system sees aligned reads of whole sectors whatever offsets
the caller asks for, and the next block in sequence costs no seek (a seek back on FAT walks the
cluster chain from the start of the file). A writer collects data until the buffer is full. Both are
move-only and close on destruction (a writer aborts); each is used by one thread at a time. An open
reader or writer holds a file slot of the file system (`FS_LITTLEFS_NUM_FILES`,
`FS_FATFS_NUM_FILES`; `OpenRead` returns `-ENOMEM` when none is free).

They work on the file handle directly, outside the service's mutex. littlefs locks each call per
mount; FatFs does only with `FS_FATFS_REENTRANT` (Zephyr's default is n), which a FAT volume needs
whenever another thread uses it at the same time, as the log writer does. A write that fails or falls
short (FAT reports a full volume as a short write, which counts as `-ENOSPC`) latches: later appends
return the same error and `Commit()` refuses to publish a file with a hole.

**Staged writes.** `BeginWrite()` opens `.assets_tmp/upload.part` at the root of the store's volume
(creating the directory, truncating a stale file); one write is staged per volume at a time, for
whichever store. `Commit()` writes out the buffer, closes the file, creates the store's directory
when it is missing and renames the staged file onto the asset (`IFsService::Rename`), replacing
the old one; when that fails the staged file is deleted. `Abort()` closes and deletes it.
`RemoveStaged()` deletes one that a power loss left.

**`AssetProvider`** is what a unit serves of one kind: the kind and its name (`"images"`), the store,
the extensions with their dot, the allowed operations as bits (`READ` 1, `WRITE` 2, `DELETE` 4), the
largest upload (`max_size`, 0 for a kind that takes none), an optional `is_in_use(operation,
name)` that refuses an operation on a file and an optional span of `built_ins`. The `is_in_use` callback
runs on the configuration work queue without the registry's lock and must not take the LVGL lock.
`MakeScriptProvider()` builds the two script kinds.

A **built-in** (`BuiltInAsset{name, data}`) is a file of the kind that is part of the firmware: bytes in
static storage (flash) that the `assets` group serves as a read-only file, in place, with no copy and no
volume (rt_gauge's three fonts). Its name must be valid for the kind and unique ignoring ASCII case, or
`Register()` refuses the provider; a stored file of the same name is hidden by it. The group lists a
built-in with `builtin: true` and answers `BUILT_IN` to a `WRITE`, `DELETE` or `RENAME` of its name (see
[smp.md](smp.md#assets-asset-kinds)). `FindBuiltIn(provider, name)` finds one by name.

**`AssetsManager`** is the registry: at most one provider per kind. `Register()` refuses a kind that
is registered already, a provider without a store and one whose built-in names are invalid or repeat;
`Unregister()` never throws, so destructors call it; `Find()` and `GetProviders()` (ordered by kind) hand out shared pointers, so a caller keeps
the provider it found while another thread unregisters it. It is thread-safe: widgets call `Find()`
under the LVGL lock and the SMP group from the configuration work queue, and its mutex is never held
while a provider's callback or a referrer runs. It holds a kernel mutex, so create it with
`std::make_shared`. The composition root creates one after the file systems and passes it to every
controller that registers a kind and to the `ManagementController` that serves them; like the
configuration work queue it is shared, not owned by one controller.

The rt_core controllers register their kind at the end of a successful `Initialize()`, when they
have a registry and an SD service, and take it back, with their referrers, when they are destroyed
(`LoggingController::Teardown()`); without `EERIE_LEAP_ASSETS` they accept the registry and ignore
it. The `LoggingController` registers `logs` in the writer role only. rt_gauge's `UiController`
registers its three kinds on the internal volume when it initializes and removes them when it is
destroyed.

| Kind | Name | Store (volume, directory) | Extensions | Access | Max | In use | Registered by |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `images` | internal, `ui_assets/images` | `.bin` | R W D | rt_gauge's `EERIE_LEAP_UI_IMAGE_MAX_SIZE` (4 MiB) | never | rt_gauge `UiController` |
| 1 | `animations` | internal, `ui_assets/animations` | `.json` | R W D | rt_gauge's `EERIE_LEAP_UI_ANIMATION_MAX_SIZE` (512 KiB) | never | rt_gauge `UiController` |
| 2 | `sensor_scripts` | SD, `scripts/sensors` | `.lua` | R W D | `EERIE_LEAP_SENSOR_SCRIPT_MAX_SIZE` | `DELETE` of a script a stored configuration refers to | `SensorsController` |
| 3 | `can_scripts` | SD, `scripts/can` | `.lua` | R W D | `EERIE_LEAP_CAN_SCRIPT_MAX_SIZE` | `DELETE` of a script a stored configuration refers to | `CanbusController` |
| 4 | `logs` | SD, `logs` | `.mf4` | R D | 0 | `READ` and `DELETE` of the file being written | `LoggingController`, writer role |
| 5 | `fonts` | internal, `ui_assets/fonts` | `.ttf` | R W D | rt_gauge's `EERIE_LEAP_UI_FONT_MAX_SIZE` (256 KiB) | never | rt_gauge `UiController` (and its three built-in fonts, read-only, from flash) |

The kind numbers go over the wire: append only. Widgets find their images, animations and fonts through the
registry by kind (`FILE_PATH` is a path relative to the kind's directory, a bare name for a file a
client uploaded); a client that overwrites or deletes a file a widget shows changes it at the widget's
next render.

**Store generation and checkpoint.** `AssetStore::GetGeneration()` changes whenever the files of a
store may have: the upper 32 bits are the volume's mount generation (`IFsService::GetMountGeneration()`,
which counts mounts, so an SD card edited on a PC ends it; 0 always for a service that cannot tell),
the lower 32 a write counter that `Save()`, `Delete()` and `AssetWriter::Commit()` increase, whether or
not they succeed. The SMP `STAT` keeps the CRC of a file as a checkpoint taken at a generation; it is
current only while the generation is the same, and a same-size overwrite through the store ends it. A
commit of an upload seeds the checkpoint with the CRC it verified, so a `STAT` after an upload reads
nothing. This assumes that assets are written only through their store.

**Rename.** The SMP `RENAME` moves a file within its kind (`AssetStore::Rename()`, which increases the
generation also when it fails: FAT deletes the target before it renames). The checkpoint of the source
follows the file when it is current, since the bytes are the same; one of the target is dropped. The
`images`, `animations` and `fonts` kinds have no in-use check, so a widget showing the renamed file keeps its
loaded copy and shows nothing after its next rebuild, as after `DELETE`.

**Script references.** A controller that owns stored script paths adds a referrer with
`AddReferrer(owner, fn)`, which says whether its configuration refers to a path relative to the
volume. `SensorsController` answers for every sensor's `script_path`, enabled or not,
`CanbusController` for every message's over every channel. Paths compare ignoring ASCII case, as FAT
does, skipping empty names and `.`; an empty path matches nothing. They read the configuration in
effect in memory (the default, when the stored one failed to load); none (a null `Get()`) refers to
nothing, and an exception while reading it counts as referring, so a script that might be in use is
kept. Both script kinds ask every
referrer (`IsReferenced(path)`), since a sensor may name a script in `scripts/can` and a message one in
`scripts/sensors`: a referenced script cannot be deleted (`IN_USE`), but it can be read and
overwritten. A script outside the two directories still runs when a `script_path` names it; it is
not listed or served.

**Names.** A name is 1 to `EERIE_LEAP_ASSETS_MAX_NAME_LENGTH` bytes of `A-Z a-z 0-9 . _ -`, not
starting with a dot and ending in one of the kind's extensions (compared ignoring ASCII case) with at
least one character before it (`AssetStore::IsValidName()`). There is no separator, so a name cannot
leave its directory. The writer's log names (`EERIE_LEAP_LOG_DATA_FILE_PREFIX` plus 26 characters, see
[logging.md](logging.md#how-it-works)) must pass it; the build checks that.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_ASSETS` | n | Builds the assets; selects `EERIE_LEAP_FS`. |
| `EERIE_LEAP_ASSETS_MAX_NAME_LENGTH` | 8..128, 64 | Longest asset name, in bytes. |
| `EERIE_LEAP_SENSOR_SCRIPT_MAX_SIZE` | 1..1048576, 65536 | Largest sensor script an upload may bring. |
| `EERIE_LEAP_CAN_SCRIPT_MAX_SIZE` | 1..1048576, 65536 | Largest CAN script an upload may bring. |

## Directory layout

| Path | Filesystem | Written by |
| --- | --- | --- |
| `config/<type>_config.cbor` | internal | Configuration managers |
| `ui_assets/images/*.bin`, `ui_assets/animations/*.json`, `ui_assets/fonts/*.ttf` | internal | Clients, as the `images`, `animations` and `fonts` kinds; rt_gauge's test setup writes its demo files there |
| `scripts/sensors/*.lua`, `scripts/can/*.lua` | SD | Clients, as the `sensor_scripts` and `can_scripts` kinds; referenced by sensors and CAN messages through `script_path`. A `script_path` may name any file on the card, and a script outside these directories still runs but is not listed. |
| `logs/log_data_<YYYYMMDD>T<HHMMSS>Z_<rand4>.mf4` | SD | Log writer (`LogFileNaming::LOGS_DIR`, `EERIE_LEAP_LOG_DATA_FILE_PREFIX`); UTC start time; the `logs` kind. Logs written before the directory was fixed stay in `data_logs/` and are not listed. |
| `logs/end_times.txt` | SD | Log writer (`LogEndTimes`): the end time of each finalized log, a line `<end_ms> <name>` per file; not a log, so the `logs` kind never serves it (see [logging.md](logging.md#log-files-as-assets)). |
| `.assets_tmp/upload.part` | each volume that takes uploads | The upload being staged; renamed onto the asset or deleted, and a stale one is deleted when the `assets` group starts. |

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
`GetTotalSpace`, `GetUsedSpace` and `Format`. All calls hold one mutex per service.

Zephyr's FAT rename deletes the target first, so a file renamed onto itself is lost. `Rename`
therefore takes only plain paths, `A-Z a-z 0-9 . _ -` and `/` with no name ending in a dot, and
refuses a pair that names one file ignoring ASCII case, both with `-EINVAL`: within that alphabet
FAT and littlefs resolve a name as written, while FatFs folds other spellings onto one name (a
backslash is a separator, trailing dots and spaces are dropped, a control byte ends the path).

Two instances exist in the application:

| Instance | Mount | Holds |
| --- | --- | --- |
| Internal (`FsService`) | alias `intfs0`, littlefs on internal flash | `config/*.cbor` |
| SD card (`SdmmcService`) | alias `sdfs0` on the `zephyr,sdhc` disk, FAT | Lua scripts, `logs/` |

The SD service is optional for the sensors and CAN code, which accept a null SD service and then
skip script validation. The writer-role `LoggingController` requires one, and its `Initialize()`
fails without it, so a composition root with no SD service does not build it (rt_daq skips it). The
remote role ignores the SD service. See [logging.md](logging.md#wiring).

Clients list a directory of the card, such as `scripts/`, with the SMP `files` group, which the
`ManagementController` serves with the SD service it is given (see [smp.md](smp.md)).

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

`AssetsManager(fs_service, assets_dir)` stores opaque blobs under one directory of a filesystem
service: `Save`, `Load` (into a PMR vector), `Delete`, `Exists`. Selects `EERIE_LEAP_FS`; no
other options.

## Directory layout

| Path | Filesystem | Written by |
| --- | --- | --- |
| `config/<type>_config.cbor` | internal | Configuration managers |
| `scripts/*.lua` (by convention) | SD | The user; referenced by sensors and CAN messages through `script_path` |
| `logs/log_data_<YYYYMMDD>T<HHMMSS>Z_<rand4>.mf4` | SD | Log writer (`LogFileNaming::LOGS_DIR`, `EERIE_LEAP_LOG_DATA_FILE_PREFIX`); UTC start time. Logs written before the directory was fixed stay in `data_logs/` and are not listed. |

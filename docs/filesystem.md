# Filesystem and assets

Code: `src/subsys/fs/`, `src/subsys/assets/`.

## Filesystem service (`EERIE_LEAP_FS`)

`FsService` wraps one Zephyr `fs_mount_t` taken from the device tree `fstab` (see
[device-tree.md](device-tree.md)): the entry itself, not a copy, because `fs_mount()` links it
into the global mount list. Every path is relative to the mount point and limited to 256 bytes
with the mount prefix. The interface (`IFsService`) offers `WriteFile` (overwrite or append),
`ReadFile`, `OpenFile`, `CreateDirectory`, `Exists`, `DeleteFile`, `DeleteRecursive`,
`ListFiles`, `GetFileSize`, `GetTotalSpace`, `GetUsedSpace` and `Format`. All calls hold one
mutex per service.

`FsServiceStreamBuf` is a `std::streambuf` over a file of a service (read, write or append,
4 KiB buffer, seekable). The MDF4 logger writes through it.

Two instances exist in the application:

| Instance | Mount | Holds |
| --- | --- | --- |
| Internal (`FsService`) | alias `intfs0`, littlefs on internal flash | `config/*.cbor` |
| SD card (`SdmmcService`) | alias `sdfs0` on the `zephyr,sdhc` disk, FAT | Lua scripts, `data_logs/`, log metadata |

The SD service is optional: the sensors, CAN and logging code accept a null SD service and then
skip script validation and logging.

## SD card service

`SdmmcService` extends `FsService` for a removable card:

- `IsAvailable()` is true while the card is present and mounted. Presence comes from the
  registered handler (`DtFs::IsSdCardPresent`): native card detection on SDHC hosts, a status
  command on SPI hosts, which cannot detect the card otherwise.
- `SdMonitorStart()`/`SdMonitorStop()` run a thread (priority 2) that polls presence every
  `EERIE_LEAP_SD_CHECK_INTERVAL_MS` and mounts or unmounts accordingly. The application does
  not start it on SPI hosts, where the status command interferes with logging.

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
| `data_logs/log_data_<ms>_<rand>.mf4` | SD | Log writer (`EERIE_LEAP_LOG_DATA_FILES_DIR`, `_PREFIX`) |
| `logs_metadata/log_metadata` | SD | Simple logger metadata (`EERIE_LEAP_LOG_METADATA_FILE_*`) |

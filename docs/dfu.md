# Firmware update (DFU)

A unit keeps two firmware images and takes a new one over SMP: from a phone over BLE, from a PC on
the bench console or the CAN bus, or through the access point (AP) from the phone to another unit on
the CAN bus. The update goes into the inactive slot; the previous image stays as the backup, so a new
image that does not come up healthy reverts by itself.

It is MCUmgr's standard image management (`img_mgmt`, group 1) and `os` reset (group 0) on top of an
MCUboot bootloader, plus what rt_core adds: an identity check on every upload, the [`dfu` group](#the-dfu-group-72)
and the [confirmation of a new image](#confirmation).

Code: `src/subsys/dfu/` (identity, upload guard, confirmation), `src/subsys/dfu/smp/` (the group),
`cmake/` (identity TLV and signing), `keys/dev-ec-p256.pem` (the committed development key).
Kconfig: `src/subsys/dfu/Kconfig.dfu`. The partition tables, the sysbuild files and MCUboot's own
configuration belong to the application.

## Modes and slot rotation

MCUboot runs in **swap-using-move** mode with revert (Espressif's default for these SoCs):

1. Slot 0 always runs. An update is uploaded into slot 1.
2. The client marks it *test* (`img_mgmt` state write, without `confirm`) and resets the unit.
3. MCUboot swaps the slots: the new image moves to slot 0, the old one to slot 1, where it stays as
   the backup. The swap takes about three erase-and-write steps per 4 KB of image, so minutes on a
   gauge, with the display dark; a power cut during it is safe, MCUboot resumes on the next boot.
4. The new image has to **confirm** itself before the next reset, or MCUboot swaps back. A unit
   confirms itself once it is healthy (see [Confirmation](#confirmation)); a client may confirm earlier.
5. After a confirmed update the backup lies where MCUboot expects an upgrade (slot 1, first sector),
   so the client can mark it *test* again: a **manual rollback** without uploading anything.

Why not swap-using-offset: its backup does not lie where an upgrade is looked for, so a confirmed
update could not be rolled back by hand. Direct-XIP does not fit the ESP32, whose application maps its
own flash from the slot it was linked for: it would need one build per slot.

An image runs only from slot 0. The signing wrapper (`cmake/sign_image.cmake`) hands imgtool the
number of sectors of slot 0 (`--max-sectors`), so that it refuses an image that does not fit once the
swap trailer is reserved.

Test builds on native_sim set `EERIE_LEAP_DFU` with or without `BOOTLOADER_MCUBOOT`: `img_mgmt` and the
image manager run there on the flash simulator, in either swap mode.

## Layout rules

The partition table is the application's, one for the app and the same one for MCUboot (each board has
a complete table that both include). The rules the DFU code relies on:

- `slot0_partition` and `slot1_partition` exist and have the same size.
- Every slot starts on a 64 KB boundary (the ESP32's MMU page), which keeps direct-XIP possible later
  without reflashing over USB.
- The largest image is `(sectors of a slot - 1) * sector size - trailer`: swap-using-move costs one
  sector plus the status area. With `MCUMGR_GRP_IMG_TOO_LARGE_SYSBUILD` (the default under MCUboot built
  by sysbuild) the unit refuses a larger upload with the first chunk (`img_mgmt` error
  `INVALID_IMAGE_TOO_LARGE`); a larger `len` than the slot is refused in any case.
- The internal file system sits elsewhere on the flash. Changing the table, the mode or the key means
  reflashing MCUboot, which is not updated over the air; it goes on over USB.

## Image identity

The firmware of one product runs on several boards (the gauge's S3 and P4 builds share a product ID),
so an image says what it is built for, and a unit only takes an image made for it.

### The TLV

The signing step adds a **protected** (signed) MCUboot TLV of type `0x00A0` (imgtool's vendor range):

```
[format u8 = 1][product_id u16 LE][board_len u8][board_target, board_len ASCII bytes]
```

`product_id` is `CONFIG_EERIE_LEAP_PRODUCT_ID` and `board_target` Zephyr's `CONFIG_BOARD_TARGET`, for
example product `0xA401` and `esp32s3_touch_amoled_1_75/esp32s3/procpu`:
`01 01 a4 28 "esp32s3_touch_amoled_1_75/esp32s3/procpu"`. The board is printable ASCII, at most 116
bytes: the identity alone fills the 128 bytes of protected TLVs a unit accepts (4 for the area's info
header, 4 for the TLV's header, 4 for the fixed part).

`cmake/image_identity.cmake` builds the value (`eerie_leap_image_identity_hex()`; also runnable as
`cmake -DPRODUCT_ID=... -DBOARD_TARGET=... -P`), and `cmake/sign_image.cmake` adds it to
`CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS` as `--custom-tlv 0xa0 0x<hex>` for every signed image of the
build. `ImageIdentity` (`src/subsys/dfu/image_identity.h`) is the same format in C++: `Create()`,
`Encode()`, `Parse()`, and `Check()` against the unit's own.

### The check while the image arrives

`ImageUploadGuard` registers on `MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK`, the one `img_mgmt` hook that can
refuse a chunk (the state write behind *test* and *confirm* only notifies). `img_mgmt` calls it for
every chunk, before it writes it. The chunks go through an `ImageIdentityCollector`:

1. The first chunk holds the 32-byte MCUboot header (`ih_magic` `0x96f3b83d`, `ih_hdr_size`,
   `ih_protect_tlv_size`, `ih_img_size`, little-endian). The protected TLVs lie at
   `[ih_hdr_size + ih_img_size, + ih_protect_tlv_size)`. An image with no protected TLVs
   (`NO_IDENTITY`), with more than 128 bytes of them, a bad header, or a declared `len` that cannot hold
   them (`BAD_IDENTITY`) is refused with its first chunk, so a foreign or old image costs one round trip.
2. The bytes of that range are collected into a fixed 128-byte buffer as the chunks pass, across chunk
   boundaries. Chunks come in order; one sent again is harmless, one that skips data fails the check.
3. When the range is complete (in the last chunk or the one before it) its TLVs are walked: the
   info header (`0x6908`, total size), each entry (`u16 type, u16 len, value`). The identity (`0x00A0`)
   is parsed and compared with the unit's own, the product first.

| Result | Meaning | `dfu` error |
| --- | --- | --- |
| match | The image is for this product and board. | none |
| `NO_IDENTITY` | No protected TLVs, or none is the identity. | 2 |
| `WRONG_PRODUCT` | Another product. | 3 |
| `WRONG_BOARD` | This product, another board. | 4 |
| `BAD_IDENTITY` | Malformed header, TLV area (info header, lengths), identity value (format, lengths, characters), a second identity, a gap in the chunks, data beyond `len`, or more than 128 bytes of protected TLVs. | 5 |

A refusal answers the chunk with an SMP v2 error `{group: 72, rc: <error>}` and:

- resets `img_mgmt`'s upload state, so that a resume with the same `sha` starts over instead of
  continuing into the erased image;
- when an earlier chunk of this upload was accepted, **erases the sector of the slot that holds the
  image header** (at sector 1 with swap-using-offset), so that the partial image can be neither listed
  nor marked *test*. A refusal of the first chunk wrote nothing and leaves the slot as it was: it can
  hold the backup image.

The callback runs on the MCUmgr work queue: no allocation, no exceptions. MCUmgr passes callbacks no
context, so one guard can be registered. The identity is covered by the image signature, so a signed
image cannot be given another identity; the check is a safety net against flashing the wrong build,
not a security boundary (a genuine image of this unit, including an older one, is accepted).

Clients check first: the identity of the file against `device info`'s `product` and `board`, so that a
mismatch normally never starts an upload. The unit's check also covers clients that skip that step.

## The `dfu` group (72)

`SmpGroupId::DFU`, served by `DfuMgmtGroup` (`src/subsys/dfu/smp/`) from an `IDfuStatusSource`.

| Command | ID | Request | Response |
| --- | --- | --- | --- |
| `STATUS` (read) | 0 | | `{confirmed: bool, state: uint, confirm_in_ms?: uint, reset_cause: uint}` |

- `confirmed`: whether the running image is confirmed (permanent).
- `state`: `CONFIRMED` 0 (nothing is armed), `VERIFYING` 1 (a test boot, waiting for boot complete),
  `CONFIRMING` 2 (boot complete, the unit confirms itself after the delay), `FAILED` 3 (the deadline
  passed or a probe stuck; a reset and MCUboot's revert follow).
- `confirm_in_ms`: the time left until self-confirmation, only in `CONFIRMING`.
- `reset_cause`: the `hwinfo` reset cause mask of this boot (for example the watchdog bit after a
  revert, which a client reports when the old version is active again).

Errors: the group's own commands have none. The codes of the identity check above are sent by the
upload hook in group 72, as `{err: {group: 72, rc}}` in the answer to an `img_mgmt` upload; for SMP v1
clients they translate to `EINVAL`.

`img_mgmt` reports slots, versions and flags but not this state machine, and gives an application no
error codes of its own; that is why the group exists. `device info` (65) carries the `board` next to
`product`, the two values a client compares an image's identity with.

## What `img_mgmt` is set up to do

`EERIE_LEAP_DFU` selects the pieces and rt_core's defaults keep them small:

- `IMG_ERASE_PROGRESSIVELY`: a sector is erased when the upload reaches it, so no request waits for
  the erase of the whole slot.
- `IMG_ENABLE_IMAGE_CHECK`: the client sends the SHA-256 of the whole file as `sha` with the first
  chunk. The unit answers an upload of an image that is in the slot already with the full offset, lets
  a client resume at offset 0 with the same `sha` (the answer carries the unit's offset), and checks the
  slot against the hash after the last chunk (`match` in the answer; allow about 5 s for it).
- `MCUMGR_GRP_IMG_UPLOAD_CHECK_HOOK`, `MCUMGR_GRP_IMG_STATUS_HOOKS`, `MCUMGR_MGMT_NOTIFICATION_HOOKS`:
  the identity check above, and the notifications the logging controller and the confirmation use.
- `MCUMGR_GRP_IMG_MUTEX`: the upload state can be reset from the guard.
- `MCUMGR_GRP_OS_RESET_HOOK`: a unit that writes logs ends its log before it resets.
- `MCUMGR_GRP_OS_BOOTLOADER_INFO` under MCUboot: `os` bootloader info.
- `MCUMGR_GRP_IMG_VERBOSE_ERR` when MCUmgr supports the original protocol: a reason text with the
  `img_mgmt` errors of SMP v1 clients (a few hundred bytes of flash).

Uploading to a unit while another client uploads makes `img_mgmt` restart the transfer; clients do not
share a unit during an update.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DFU` | y with `BOOTLOADER_MCUBOOT` | Image management. Depends on `EERIE_LEAP_SMP`, `FLASH` and `FLASH_MAP`. Selects `STREAM_FLASH`, `IMG_MANAGER`, `IMG_ERASE_PROGRESSIVELY`, `IMG_ENABLE_IMAGE_CHECK`, `MCUMGR_GRP_IMG`, `MCUMGR_GRP_IMG_MUTEX`, `MCUMGR_GRP_IMG_VERBOSE_ERR`, `MCUMGR_MGMT_NOTIFICATION_HOOKS`, `MCUMGR_GRP_IMG_UPLOAD_CHECK_HOOK`, `MCUMGR_GRP_IMG_STATUS_HOOKS`, `MCUMGR_GRP_OS_RESET_HOOK`, `MCUMGR_GRP_OS_BOOTLOADER_INFO`. `IMG_MANAGER`'s choice already defaults to `MCUBOOT_IMG_MANAGER`, which cannot be selected. Default `MCUMGR_GRP_IMG_TOO_LARGE_CHECK` is `MCUMGR_GRP_IMG_TOO_LARGE_SYSBUILD` with `BOOTLOADER_MCUBOOT` (set by redeclaring the choice). |
| `MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE` | 4096 with `EERIE_LEAP_DFU` | `img_mgmt` runs on the MCUmgr work queue: bootutil trailer reads, stream flash writes and the SHA-256 image check do not fit 2048 bytes. To be trimmed after measuring on hardware. |
| `EERIE_LEAP_CONTROLLER_DFU` | y with `EERIE_LEAP_DFU` | The controller: registers the guard and the group, confirms a new image. |
| `EERIE_LEAP_DFU_SELF_CONFIRM` | y | n leaves confirmation to the client (bench), which is how the revert path is tested. |
| `EERIE_LEAP_DFU_BOOT_DEADLINE_MS` | 60000 | Boot to boot complete, for an unconfirmed image. |
| `EERIE_LEAP_DFU_CONFIRM_DELAY_MS` | 20000 | Boot complete to confirmation. |
| `EERIE_LEAP_DFU_WATCHDOG_TIMEOUT_MS` | 8000 | Hardware watchdog while unconfirmed. `wdt_esp32` programs two stages and counts 0.5 ms (S3) or 1 ms (P4) ticks, so the real reset time has to be measured. |
| `EERIE_LEAP_DFU_BENCH_FAULT_*` | none | Bench builds that fail on purpose (crash, hang, never report boot complete), for the revert drills. Never for a shipped build. |
| `EERIE_LEAP_DFU_LOG_LEVEL` | | Log level of the DFU modules. |

## Client flow

The configurator app and the bench client follow the same sequence. "Unit" is the target, reached
through the AP's route on BLE or by its CDMP ID on CAN.

1. **Pick.** `network devices` on the AP, `device info` on the unit (`product`, `board`, `sw`, `build`,
   `uid`), and `img_mgmt` state read (the slots, their versions and flags).
2. **Check the file.** Its identity TLV must match `product` and `board`; compare the versions and warn
   about a downgrade. Refuse if slot 1 is in use: a test image is running and slot 1 holds the backup
   (`img_mgmt` refuses it too).
3. **Size the chunks.** `os mcumgr params` gives `buf_size` and `buf_count`. A chunk is `buf_size - 64`
   bytes. The window is **1 for a unit behind CAN** (see [smp.md](smp.md#transports)), otherwise
   the smaller of the BLE `info` window and `buf_count - 1`.
4. **Upload.** `img_mgmt upload` with `image 0`, `len`, `sha` (SHA-256 of the file) and `off`/`data`.
   After a lost connection start again at `off 0` with the same `sha`; the unit answers with its offset.
   The last chunk waits for the slot check (allow about 5 s). Errors in group 72 mean a wrong image.
5. **Mark test.** `img_mgmt` state write with the image's `hash` and `confirm: false`.
6. **Reset.** `os reset`. Update the AP last: its reset drops the BLE link.
7. **Wait for the swap.** The unit is silent for it, minutes for a gauge. Find it again by **UID**: after
   the reboot it claims a CDMP ID again and may get another one. Poll `network devices` on the AP.
8. **Verify.** `dfu STATUS` goes `VERIFYING`, `CONFIRMING`, `CONFIRMED`, and the `img_mgmt` list shows
   the new version active and confirmed; the client may confirm earlier itself. If the old version is
   active and the new one sits unconfirmed in slot 1, the update was reverted: report a failure with
   `reset_cause`. An ignition cycle within a minute of an update undoes it the same way.
9. **Roll back (optional, any time later).** Mark the slot 1 backup *test*, reset, and wait as in 7 and 8.

## Confirmation

The confirmation of a new image (the watchdog, the liveness probes, the boot deadline and the
self-confirmation delay of the table above) is done by `DfuController` and is described here with it.
`dfu STATUS` reports where it stands.

## Tests

- `unit.subsys.dfu`: `ImageIdentity` and `ImageIdentityCollector`: encode and parse, the protected TLV
  range split at every byte offset across two chunks, missing, oversize and malformed areas, a wrong
  product or board.
- `functional.dfu` (native_sim; scenarios with MCUboot's swap-using-move mode, with swap-using-offset, and with no
  MCUboot in the configuration): real `img_mgmt` on
  the flash simulator with small slots (`boards/native_sim.overlay`), the upload guard and the `dfu`
  group. The test images are signed at configure time with MCUboot's `imgtool` and the development key,
  one per identity case. It covers an accepted upload and the listing, each refusal and the erased
  header sector, the resume, the size checks, the state write giving `BOOT_SWAP_TYPE_TEST`, and the
  `dfu STATUS` answers.
- `functional.subsys.smp_ble`: a 1024-byte group-1 packet passes the BLE router unchanged.

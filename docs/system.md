# System domain

Product identity and the per-unit system configuration. Code: `src/domain/system_domain/`.

## Product identity (Kconfig)

Compiled into `ProductInfo` and `SystemConfiguration`; advertised over BLE and reported by the
SMP `device` group.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_PRODUCT_ID` | 0..65535, 0 | 16-bit product ID (rt_daq uses 0xC401). |
| `EERIE_LEAP_REVISION` | 0..255, 0 | Hardware revision. |
| `EERIE_LEAP_HW_VERSION_MAJOR/MINOR` | 0..255 | Packed as `major << 24 \| minor << 16 \| patch`. |
| `EERIE_LEAP_HW_VERSION_PATCH` | 0..65535 | |
| `EERIE_LEAP_SW_VERSION_MAJOR/MINOR/PATCH` | 0..255, 0..255, 0..65535 | Software version, formatted `major.minor.patch.build`. |
| `EERIE_LEAP_SW_VERSION_BUILD` | 0..255 | Build number. |

The software version has one source, the application's `VERSION` file (`VERSION_MAJOR`,
`VERSION_MINOR`, `PATCHLEVEL`, `VERSION_TWEAK`), which Zephyr reads as Kconfig macros. The four
symbols default to those values, and MCUboot's image version (`MAJOR.MINOR.PATCHLEVEL+TWEAK`) comes from
the same file, so the SMP `device` group and the image report the same numbers. The build number is the
tweak. An application without a `VERSION` file, such as a test suite, gets 0 for each unless it sets the
symbols. Do not set them in `prj.conf` in an application that has the file.

The signed image also carries the unit's identity, `EERIE_LEAP_PRODUCT_ID` and the board target
(`CONFIG_BOARD_TARGET`, for example `esp32s3_touch_amoled_1_75/esp32s3/procpu`), in a protected MCUboot
TLV (tag 0xA0): `[format u8 = 1][product u16 LE][board_len u8][board bytes]`. `cmake/sign_image.cmake` adds
it with `--custom-tlv`, and `--max-sectors` from the size of `slot0_partition`, through
`CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS` when `CONFIG_BOOTLOADER_MCUBOOT` is set. `cmake/image_identity.cmake`
builds the same bytes for a script that signs an image the same way.

## Unit features

The features are the one description of what a unit can do: a 16-bit `ProductFeature` mask
(`models/product_feature.h`) that the SMP `device` group, the BLE advertisement and CDMP (heartbeat
and discovery response) all carry. There is no product family and no CDMP device type. The
`EERIE_LEAP_FEATURES_*` options sit in their own menu, "EerieLeap unit features", and default from
the domains the build contains; a board can turn one off. `BUILD_FEATURES` is the mask they make.

| Symbol | Bit | Default |
| --- | --- | --- |
| `EERIE_LEAP_FEATURES_ANALOG_INPUTS` | 0 | y with `EERIE_LEAP_CONTROLLER_ADC` (the ADC domain alone, which `EERIE_LEAP_DOMAIN_SENSOR` selects, is not an input). |
| `EERIE_LEAP_FEATURES_DIGITAL_INPUTS` | 1 | y with `EERIE_LEAP_DOMAIN_SENSOR` and `EERIE_LEAP_GPIO`. |
| `EERIE_LEAP_FEATURES_CANBUS` | 2 | y with `EERIE_LEAP_DOMAIN_CANBUS`. |
| `EERIE_LEAP_FEATURES_LOGGING` | 3 | y with `EERIE_LEAP_DOMAIN_LOGGING`. |
| `EERIE_LEAP_FEATURES_BLUETOOTH` | 4 | y with `EERIE_LEAP_DOMAIN_BLE`. |
| `EERIE_LEAP_FEATURES_DISPLAY` | 5 | n; set by hand. |
| `EERIE_LEAP_FEATURES_STORAGE` | 6 | y with `EERIE_LEAP_DOMAIN_LOGGING`, whose log writer needs a card; a unit with an SD service and no logging sets it by hand. It says the unit has an SD service, so it runs the scripts a `script_path` names ([scripting.md](scripting.md)), its `files` group lists the card and its `assets` group serves the kinds on it (the script kinds and, with a log writer, `logs`; [smp.md](smp.md)); whether a card is mounted is runtime state, which the `files` group reports as `NO_CARD` and the `assets` group as `UNAVAILABLE` (`avail` false in `KINDS`). |

The bits follow the build and enable no code. The one runtime bit is LOGGING on CDMP, which a unit
sends only while it has a log writer (see [cdmp.md](cdmp.md)); the SMP `device` group reports what
the unit sends on CDMP, and the build's features on a unit without CDMP.

## Stored configuration

Schema (`system_config.cddl`):

```
CborSystemConfig = [ device_id: uint .size 8, build_number: uint ]
```

| Field | Default | Meaning |
| --- | --- | --- |
| `device_id` | random 64-bit, generated once from the secure RNG | Identifies the unit for the lifetime of its internal filesystem. |
| `build_number` | 0 | Updated by the application through `UpdateBuildNumber()`; saved only when it changes. No longer reported: the `device` group and the formatted version use `EERIE_LEAP_SW_VERSION_BUILD`. |

There are no validation rules. The file is `config/system_config.cbor`.

## SMP `device` group

Command `INFO` (read) returns `{product, revision, features, hw, sw, build, board}` (`build` is
`EERIE_LEAP_SW_VERSION_BUILD`, the `VERSION` file's tweak; `board` is the text `CONFIG_BOARD_TARGET`), on a unit
with CDMP `{cdmp_id, uid, status}`, and on a unit with sensors
`{adc_channels, gpio_channels, sensors_max}`, which the application passes to the
`ManagementController` from `SensorsController::GetCapabilities()`. Served by the
`ManagementController`; see [smp.md](smp.md).

## Wiring

`SystemController` (`EERIE_LEAP_CONTROLLER_SYSTEM`, depends on `EERIE_LEAP_DOMAIN_SYSTEM` and
`EERIE_LEAP_FS`) creates the manager on the internal filesystem with the configuration work
queue. It is the first controller the application creates, and its manager is registered with
`ConfigurationService` under type 1. The constructor logs the hardware and software versions and
the device ID.

# System domain

Product identity and the per-unit system configuration. Code: `src/domain/system_domain/`.

## Product identity (Kconfig)

Compiled into `ProductInfo` and `SystemConfiguration`; advertised over BLE and reported by the
SMP `device` group.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_PRODUCT_FAMILY_*` (choice) | `NONE` | `NONE` (0), `PROCESSING_MODULE` (1), `DISPLAY_MODULE` (2), `INPUT_MODULE` (3). |
| `EERIE_LEAP_PRODUCT_ID` | 0..65535, 0 | 16-bit product ID (rt_daq uses 0xC401). |
| `EERIE_LEAP_FEATURES_ANALOG_INPUTS` | n | Feature bit 0. |
| `EERIE_LEAP_FEATURES_DIGITAL_INPUTS` | n | Bit 1. |
| `EERIE_LEAP_FEATURES_CANBUS` | n | Bit 2. |
| `EERIE_LEAP_FEATURES_LOGGING` | n | Bit 3. |
| `EERIE_LEAP_FEATURES_BLUETOOTH` | n | Bit 4. |
| `EERIE_LEAP_FEATURES_DISPLAY` | n | Bit 5. |
| `EERIE_LEAP_REVISION` | 0..255, 0 | Hardware revision. |
| `EERIE_LEAP_HW_VERSION_MAJOR/MINOR` | 0..255 | Packed as `major << 24 \| minor << 16 \| patch`. |
| `EERIE_LEAP_HW_VERSION_PATCH` | 0..65535 | |
| `EERIE_LEAP_SW_VERSION_MAJOR/MINOR/PATCH` | same | Software version, formatted `major.minor.patch.build`. |

The feature bits are a declaration for clients; they do not enable code.

## Stored configuration

Schema (`system_config.cddl`):

```
CborSystemConfig = [ device_id: uint .size 8, build_number: uint ]
```

| Field | Default | Meaning |
| --- | --- | --- |
| `device_id` | random 64-bit, generated once from the secure RNG | Identifies the unit for the lifetime of its internal filesystem. |
| `build_number` | 0 | Updated by the application through `UpdateBuildNumber()`; saved only when it changes. |

There are no validation rules. The file is `config/system_config.cbor`.

## SMP `device` group

Command `INFO` (read) returns `{family, product, revision, features, hw, sw, build}`, on a unit
with CDMP `{cdmp_id, uid, device_type, status}`, and on a unit with sensors
`{adc_channels, gpio_channels, sensors_max}`, which the application passes to the
`ManagementController` from `SensorsController::GetCapabilities()`. Served by the
`ManagementController`; see [smp.md](smp.md).

## Wiring

`SystemController` (`EERIE_LEAP_CONTROLLER_SYSTEM`, depends on `EERIE_LEAP_DOMAIN_SYSTEM` and
`EERIE_LEAP_FS`) creates the manager on the internal filesystem with the configuration work
queue. It is the first controller the application creates, and its manager is registered with
`ConfigurationService` under type 1. The constructor logs the hardware and software versions and
the device ID.

# Configuration system

Every domain with user settings (system, ADC, CAN bus, sensors, logging, the logging triggers)
stores them the same way: one CBOR file per type on the internal filesystem, validated on every
load and save, and exposed to clients through the SMP `config` group. This page describes the
shared machinery; the per-domain pages describe the content of each configuration.

Code: `src/configuration/`, `src/domain/configuration_domain/`.

## Layers

| Layer | Set by | Changes at |
| --- | --- | --- |
| Kconfig | `prj.conf`, board `.conf` | Build time: what is compiled, sizes, priorities, limits, product identity |
| Device tree | Board `.dts` / overlay | Build time: which hardware exists (ADCs, CAN controllers, GPIO inputs, filesystems, display) |
| Stored configuration | Firmware defaults, the C++ API, or an SMP client | Run time: calibrations, sensors, CAN messages, logging and its triggers |

## Storage

`CborConfigurationService<T>` (`src/configuration/services/cbor_configuration_service.h`) owns
one file: `config/<name>.cbor` relative to the internal filesystem mount point (see
[filesystem.md](filesystem.md)). It encodes and decodes with the zcbor code generated from the
schemas in `src/configuration/cbor/cbor_schemas/*.cddl`, and keeps the size and CRC32 of the
file as last saved or loaded (`GetStoredInfo()`).

When the service is given a work queue (the application's configuration work queue), every load
and save runs on that queue and the caller waits, so no caller thread needs the stack for CBOR
and filesystem work. Calls from the queue thread itself run inline.

| Type | ID | Manager | File |
| --- | --- | --- | --- |
| System | 1 | `SystemConfigurationManager` | `config/system_config.cbor` |
| ADC | 2 | `AdcConfigurationManager` | `config/adc_config.cbor` |
| CAN bus | 3 | `CanbusConfigurationManager` | `config/canbus_config.cbor` |
| Sensors | 4 | `SensorsConfigurationManager` | `config/sensors_config.cbor` |
| Logging | 5 | `LoggingConfigurationManager` | `config/logging_config.cbor` |
| UI, Display | 6, 7 | Reserved for applications | |
| Logging control | 8 | `LoggingControlConfigurationManager` | `config/logging_control_config.cbor` |

The file names are set by the controllers that create the managers. `LoggingController` registers
logging control (the logging triggers, see [logging.md](logging.md#stored-triggers)) on units with and
without a log writer, and the logging configuration only on those with one. A logging trigger names the unit it starts and stops by
its CDMP UID, 0 for every unit that can log.

## Configuration managers

Each manager derives from `CborConfigurationManagerBase<TConfiguration, TCborConfig>`
(`src/domain/configuration_domain/utilities/cbor_configuration_manager_base.h`) and provides
three things: `Serialize()` (model to CBOR struct), `Deserialize()` (CBOR struct to model) and
`CreateDefaultConfiguration()`. Both conversions validate, so nothing invalid is ever stored or
handed out.

Behaviour shared by every manager:

- **Load or default.** The constructor loads the stored file. If no file exists, the default is
  created and saved. If a file exists but fails to load (missing SD card, a stricter validator
  after an update, corrupt data), the file is kept untouched, an unsaved default is used instead,
  and `GetCborConfigurationInfo().is_applied` reports `false` until a valid configuration is
  stored.
- **`Get()`** returns the current configuration as a shared pointer. Consumers must treat it as
  immutable; a change is always a whole new object.
- **`Update(configuration)`** serializes, parses the result back (so what is kept is exactly what
  a reload would give), saves, and replaces the current object.
- **`ApplyCborConfiguration(bytes, reason)`** decodes and validates CBOR from a client, stores
  the bytes as received (so the CRC a client computed stays the stored CRC), and replaces the
  current object. On rejection `reason` receives the validator's message. A manager can also
  check a client's configuration against the rest of the unit before it is stored
  (`ValidateApply()`; the sensors manager checks CAN sensors against the CAN bus configuration).
  Loading and `Update()` skip that check, so a stored configuration still loads after what it was
  checked against has changed.
- **Update handlers.** Managers whose configuration drives a running service also implement
  `IConfigurationManager::RegisterConfigurationUpdatedHandler()`. The handler runs after a
  successful `Update()` or `ApplyCborConfiguration()`, not on the initial load. Controllers use
  it to restart the affected services (see each domain page).
- **The stored handler.** Every store, `Update()`, `Adopt()` or `ApplyCborConfiguration()`, calls
  the manager's stored handler (`SetStoredHandler()`) once it succeeded, on the storing thread and
  before the manager's updated handler. `ConfigurationService` sets it when the manager is
  registered, and passes the type on to its own stored handler (below). Not reported: a load, the
  default made while the manager is built, and a rejected or failed store. A handler that throws
  is logged and does not fail the store, which is complete by then. The handler is copied under a
  lock and called outside it, so it can be replaced from any thread while a store calls it.
- **`UpdateOwnSetting(configuration)`** stores like `Update()` but reports to neither handler. It is
  for a setting the unit keeps for itself, such as the brightness the gauge's own slider sets, which
  is no configuration update from the network and so stops no logging (see
  [logging.md](logging.md#configuration-updates)).

## Configuration service

`ConfigurationService` (`src/domain/configuration_domain/services/configuration_service.h`)
is the registry the SMP `config` group talks to. Each controller registers its manager under its
type ID with an optional size cap (default 64 KiB) that bounds the transfer buffer of an SMP
write. It routes `ApplyCborConfiguration`, `GetCborConfiguration` and `GetCborConfigurationInfo`
to the manager of a type and lists the registered types.

`RegisterConfigurationStoredHandler(handler)` takes the report of every successful store of every
registered manager, with the manager's type: an SMP write over any transport, a manager's
`Update()` or `Adopt()`, or an apply that bypasses the service (rt_daq's `logging reapply-sensors`).
There is one handler per service: a second call replaces the first, `nullptr` clears it, and one
cleared during its call finishes that call. This is how logging learns of configuration updates
(`ConfigurationUpdateLoggingStop`, see [logging.md](logging.md#configuration-updates)). Only a
manager registered with the unit's service reports: a manager that is not registered stores
silently. The gauge's system configuration is such a one, with no runtime store to report.

The `ManagementController` must be created after every manager has been registered, because the
`config` group lists the types once at start.

## SMP access

With `CONFIG_EERIE_LEAP_SMP` and `CONFIG_EERIE_LEAP_CONTROLLER_MANAGEMENT`, clients read and
write any registered type in chunks over the `config` group. The commands, errors, session rules
and timeouts are in [smp.md](smp.md).

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_CONFIGURATION` | n | Builds the storage service and the configuration domain. Selects `EERIE_LEAP_THREADING` and `CRC` (the CRC32 of each stored file). Required by every domain with stored configuration. |
| `EERIE_LEAP_SMP_CONFIG_SESSION_TIMEOUT_MS` | 5000 | Idle time after which an SMP write session or read snapshot is dropped. |

## Schemas

The CDDL files in `src/configuration/cbor/cbor_schemas/` are the wire and file format. The
generated zcbor sources next to them (`cbor_<type>_config/`) are committed; the generator
invocation is kept in the application's `CMakeLists.txt` (`generate_zcbor_configuration`,
commented out) for when a schema changes. Enumerations in the schemas are persisted integers:
new values are appended, existing values are never reordered or reused.

## Adding a configuration type

1. Write the CDDL schema and generate the zcbor code.
2. Add the model, a validator and a parser (`Serialize`/`Deserialize`) under the domain.
3. Derive a manager from `CborConfigurationManagerBase` and call `LoadOrCreateDefault()` at the
   end of its constructor, once the parser exists.
4. Register a `CborTraitRegistry` entry for the CBOR struct (see
   `src/configuration/cbor/traits/`) and add the sources to `CMakeLists.txt` under the domain's
   Kconfig symbol.
5. In the controller, create a `CborConfigurationService` with the file name and the
   configuration work queue, construct the manager, and register it with `ConfigurationService`
   under a new type ID.

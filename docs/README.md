# rt_core documentation

rt_core is the Zephyr module shared by the EerieLeap units (rt_daq, rt_gauge). It provides the
hardware subsystems, the application domains and the controllers that wire them, so an
application's `main()` only chooses what to build and in which order to start it.

## Layout

| Layer | Directory | Contents |
| --- | --- | --- |
| Subsystems | `src/subsys/` | Hardware and OS wrappers: ADC, CAN, GPIO, filesystem, assets, Bluetooth, SMP, firmware update, CDMP, Lua, expression engine, MDF, threading, time, event bus, random, CFB, device tree accessors. |
| Domains | `src/domain/` | One concern each: system, ADC, sensors, CAN bus, CAN COM, logging, scripting, BLE, configuration. A domain with user settings owns a model, a CBOR schema, a validator, a configuration manager and, with SMP, a management group. |
| Controllers | `src/controllers/` | Composition roots: each builds the services of one domain, registers its configuration manager and wires it to the others. |
| Configuration | `src/configuration/` | CBOR storage service and generated zcbor code. |

Each subsystem and domain is compiled only when its Kconfig symbol is set (`CMakeLists.txt`).

## Pages

| Page | Covers |
| --- | --- |
| [configuration.md](configuration.md) | How stored configurations are loaded, validated, saved and exposed; the shared manager behaviour |
| [device-tree.md](device-tree.md) | The aliases and chosen nodes rt_core reads, ADC channel properties, simulators |
| [adc.md](adc.md) | ADC hardware access, samples and calibration configuration, the calibration service |
| [sensors.md](sensors.md) | Sensor types, the processing pipeline, indicator conditioning, rate groups, event-driven sensors, the readings frame, sensor events |
| [canbus.md](canbus.md) | CAN buses, messages and signals, sending, reconfiguration |
| [cdmp.md](cdmp.md) | The CAN device management protocol, the COM channel, shared state, SMP over CAN |
| [smp.md](smp.md) | MCUmgr groups (`config`, `device`, `network`, `live`, `calibration`, `files`, `assets`, `logging`, `dfu`) and transports |
| [dfu.md](dfu.md) | Firmware update: MCUboot swap and revert, slot layout rules, the image identity check, the `dfu` group, confirmation, client flow |
| [bluetooth.md](bluetooth.md) | The BLE access point: GATT service, SMP routing, live data |
| [logging.md](logging.md) | Log writer, logging control, triggers and the logging controller, the MDF4 logger, logging configuration, logging status on other units |
| [system.md](system.md) | Product identity and the system configuration |
| [filesystem.md](filesystem.md) | Internal and SD filesystems, the asset store and its kind registry, directory layout |
| [gpio.md](gpio.md) | Digital inputs |
| [scripting.md](scripting.md) | Expressions and Lua scripts |
| [infrastructure.md](infrastructure.md) | Services and threads, time, event bus and bindings, random, CFB, thread priorities |

In-tree references: the CDMP protocol specification
([`src/subsys/cdmp/README.md`](../src/subsys/cdmp/README.md)) and the expression language
([`modules/expression_engine/docs/LANGUAGE.md`](../modules/expression_engine/docs/LANGUAGE.md)).
The Lua script API is part of [scripting.md](scripting.md).

## Three ways to configure

1. **Kconfig** selects what is built and sets sizes, priorities, limits and the product identity.
   Every page lists its symbols. All of them live under the "EerieLeap Core Module" menu
   (`Kconfig.rt_core_module`).
2. **Device tree** declares the hardware: which ADCs, CAN controllers, GPIO inputs, filesystems
   and display exist. See [device-tree.md](device-tree.md).
3. **Stored configuration** holds what a user changes at run time: calibrations, sensors, CAN
   messages, logging. It lives in CBOR files on the internal filesystem and is edited through the
   C++ API or by a client over SMP. See [configuration.md](configuration.md).

## Building rt_core into an application

Register the module and its two nested modules (Zephyr does not discover nested ones), then
enable the pieces:

```cmake
set(EXTRA_ZEPHYR_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/../modules/eerie_leap_rt_core/modules/zephyr_lua"
    "${CMAKE_CURRENT_SOURCE_DIR}/../modules/eerie_leap_rt_core/modules/expression_engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/../modules/eerie_leap_rt_core")
```

```
CONFIG_CPP=y
CONFIG_STD_CPP23=y
CONFIG_REQUIRES_FULL_LIBCPP=y
CONFIG_CPP_EXCEPTIONS=y
CONFIG_CPP_RTTI=y

CONFIG_EERIE_LEAP_RT_CORE=y
```

rt_core is C++23 with exceptions: errors at construction and configuration time are exceptions,
errors on sampling paths are status codes.

### Enable map

What each symbol needs. `depends` must be set by the application; `selects` is pulled in
automatically.

| Symbol | Depends on | Selects |
| --- | --- | --- |
| `EERIE_LEAP_RT_CORE` | | |
| `EERIE_LEAP_THREADING`, `_TIME`, `_RANDOM`, `_MDF`, `_CFB`, `_DEVICE_TREE`, `_GPIO`, `_ADC` | | |
| `EERIE_LEAP_FS` | | `THREADING` with `DISK_ACCESS` (the SD card service) |
| `EERIE_LEAP_EVENT_BUS` | | `THREADING` |
| `EERIE_LEAP_ASSETS` | | `FS` |
| `EERIE_LEAP_CANBUS` | `CAN` | |
| `EERIE_LEAP_BLUETOOTH` | `BT` | `THREADING` |
| `EERIE_LEAP_CDMP` | | `CRC`, `HWINFO`, `RANDOM` |
| `EERIE_LEAP_LUA_SCRIPT` | | `LUA` |
| `EERIE_LEAP_EXPRESSION_ENGINE` | | `EXPRESSION_ENGINE` |
| `EERIE_LEAP_SMP` | | `MCUMGR` and its os group, `ZCBOR`, `NET_BUF`, `CRC`, `RANDOM`, `THREADING` |
| `EERIE_LEAP_SMP_CAN` | `CANBUS` | `SMP`, `THREADING` |
| `EERIE_LEAP_SMP_BLE` | | `SMP` |
| `EERIE_LEAP_SMP_ASSETS` | `ASSETS`, `CONTROLLER_MANAGEMENT`; `FS_FATFS_REENTRANT` on FAT | `MCUMGR_SMP_CBOR_MIN_ENCODING_LEVEL_4` with `ZCBOR_CANONICAL` |
| `EERIE_LEAP_CONFIGURATION` | | `THREADING`, `CRC` |
| `EERIE_LEAP_DOMAIN_SYSTEM` | `CONFIGURATION` | |
| `EERIE_LEAP_DOMAIN_ADC` | `ADC`, `CONFIGURATION` | |
| `EERIE_LEAP_DOMAIN_SENSOR` | `CONFIGURATION`, `ADC`, `CANBUS` | `DOMAIN_SCRIPT`, `DOMAIN_ADC`, `DOMAIN_CANBUS` |
| `EERIE_LEAP_DOMAIN_CANBUS` | `CONFIGURATION`, `CANBUS` | |
| `EERIE_LEAP_DOMAIN_CANBUS_COM` | | `SMP_CAN` |
| `EERIE_LEAP_DOMAIN_LOGGING` | `CONFIGURATION`, `DOMAIN_SENSOR` | `EVENT_BUS`, `MDF`, `DOMAIN_SYSTEM` |
| `EERIE_LEAP_DOMAIN_SCRIPT` | | |
| `EERIE_LEAP_DOMAIN_BLE` | `BLUETOOTH`, `DOMAIN_SENSOR`, `DOMAIN_SYSTEM` | `SMP_BLE` |
| `EERIE_LEAP_CONTROLLER_SYSTEM` | `DOMAIN_SYSTEM`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_MANAGEMENT` | `SMP`, `DOMAIN_SYSTEM` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_ADC` | `DOMAIN_ADC`, `DEVICE_TREE`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_CANBUS` | `DOMAIN_CANBUS`, `DOMAIN_CANBUS_COM`, `DEVICE_TREE`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_SENSORS` | `DOMAIN_SENSOR`, `DOMAIN_CANBUS`, `GPIO`, `RANDOM`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_BLE` | `DOMAIN_BLE` | |
| `EERIE_LEAP_CONTROLLER_LOGGING` | `CONFIGURATION`, `EVENT_BUS`, `DOMAIN_SENSOR`, `DOMAIN_CANBUS_COM` | `THREADING` |

`EERIE_LEAP_SMP_ASSETS` is on by default wherever its dependencies are set; it adds the `assets` SMP
group to the `ManagementController` (see [smp.md](smp.md#assets-asset-kinds)).

The sensor domain needs `EERIE_LEAP_ADC` even on a unit without an ADC: the subsystem then
provides a simulator, and the sensors controller accepts a null ADC manager. It needs
`EERIE_LEAP_CANBUS`, so `CONFIG_CAN`, even on a unit without a CAN controller: CAN sensors are fed
through the CAN domain, and a configured bus whose device is missing is logged and skipped.

Log levels: `EERIE_LEAP_RT_CORE_LOG_LEVEL_*` for the module, `EERIE_LEAP_CANBUS_LOG_LEVEL_*`
and `EERIE_LEAP_LUA_LOG_LEVEL_*` for those two.

## Boot sequence

The order rt_daq's `main()` uses; dependencies flow downwards.

0. `InitializeEventChannels()`: the application bus that carries `logging_events` and
   `sensor_events`, and the `EventChannelRegistry` entries for `Sensors` and `Logging`, before
   anything publishes or subscribes (see [logging.md](logging.md#wiring)).
1. `DtConfigurator::Initialize(features)` reads the device tree.
2. Display (`Cfb`) and the application's display controller.
3. Filesystems: the SD service if a card slot exists, then the internal filesystem (fatal if it
   fails). Then the shared `AssetsManager` (`std::make_shared`), the registry of asset kinds: the
   controllers below that own a kind register it with their `Initialize()`, and the
   `ManagementController` serves what is registered (see [filesystem.md](filesystem.md#assets-eerie_leap_assets)).
4. `TimeService`.
5. The **configuration work queue** (`WorkQueueThread`, 8192 bytes, priority 5): every
   configuration load, save, SMP apply and service restart runs on it, and so does every start and
   stop of the log writer, every rebuild of the logging triggers and the remote `LOGGING` send (see
   [logging.md](logging.md#supervision) and [logging.md](logging.md#threading)).
6. `SystemController`, then `ConfigurationService` with the system manager registered.
7. `AdcController` (fatal if the ADC fails to initialize).
8. GPIO through `GpioFactory`.
9. `CanbusController` (with the SD service and the registry, for the `can_scripts` kind): initialize,
   then `Start()` the COM service.
10. The shared `SensorReadingsFrame` and `SensorsController` (the `sensor_scripts` kind). Register the processing service
    with the ADC controller (`RegisterPausedService`) and the CAN controller
    (`RegisterDependentService`).
11. The `SensorEventsPublisher` on the shared frame, started, then `LoggingController` in the
    writer role (the `logs` kind) when the SD service exists (a board with an SD file system, step 3; without a card
    in it the controller is still built and every start fails). Only a controller whose
    `Initialize()` returned 0 is kept, and its log writer joins the sensors controller's dependents
    (`RegisterDependentService`) through `GetDependentService()` (see
    [logging.md](logging.md#wiring)). A board without an SD file system builds no logging
    controller. A unit built without `EERIE_LEAP_DOMAIN_LOGGING`, such as rt_gauge, builds the
    controller in the remote role at this point instead: after `CanbusController` and
    `SensorsController::Initialize()`, before `SensorsController::Start()` and
    `ManagementController`.
12. `LoggingController::Start()`, once the last of the unit's boot stores is done (rt_daq: its
    logging setup hook; rt_gauge: its test configurations, after `SensorsController::Start()`) and
    before `ManagementController`: from here on a stored configuration stops logging on every unit
    (see [logging.md](logging.md#configuration-updates)). A board without an SD file system
    builds a `ConfigurationUpdateLoggingStop` with a sender instead, so what it stores still stops
    the units that log.
13. `ManagementController`, after every manager is registered, with the sensors' capabilities, the
    SD service and the registry.
14. `CanbusSchedulerService`, registered as a CAN dependent service.
15. `sensors_controller->Start()`, `canbus_scheduler_service->Start()`.

A BLE unit creates `BleController` after the sensors controller and starts it last.

## Tests

`west twister -T ./tests` runs the unit and functional suites on native_sim and QEMU (see the
top-level `README.md` for platforms and known gaps). The functional suites under
`tests/functional/domain/` are the executable reference for the behaviours described here.

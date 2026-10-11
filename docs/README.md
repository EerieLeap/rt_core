# rt_core documentation

rt_core is the Zephyr module shared by the EerieLeap units (rt_daq, rt_gauge). It provides the
hardware subsystems, the application domains and the controllers that wire them, so an
application's `main()` only chooses what to build and in which order to start it.

## Layout

| Layer | Directory | Contents |
| --- | --- | --- |
| Subsystems | `src/subsys/` | Hardware and OS wrappers: ADC, CAN, GPIO, filesystem, Bluetooth, SMP, CDMP, Lua, expression engine, MDF, threading, time, event bus, random, CFB, device tree accessors. |
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
| [sensors.md](sensors.md) | Sensor types, the processing pipeline, rate groups, event-driven sensors, the readings frame |
| [canbus.md](canbus.md) | CAN buses, messages and signals, sending, reconfiguration |
| [cdmp.md](cdmp.md) | The CAN device management protocol, the COM channel, SMP over CAN |
| [smp.md](smp.md) | MCUmgr groups (`config`, `device`, `network`, `live`, `calibration`) and transports |
| [bluetooth.md](bluetooth.md) | The BLE access point: GATT service, SMP routing, live data |
| [logging.md](logging.md) | Log writer, MDF4 and simple loggers, logging configuration |
| [system.md](system.md) | Product identity and the system configuration |
| [filesystem.md](filesystem.md) | Internal and SD filesystems, assets, directory layout |
| [gpio.md](gpio.md) | Digital inputs |
| [scripting.md](scripting.md) | Expressions and Lua scripts |
| [infrastructure.md](infrastructure.md) | Services and threads, time, event bus, random, CFB, thread priorities |

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
| `EERIE_LEAP_THREADING`, `_TIME`, `_RANDOM`, `_MDF`, `_CFB`, `_DEVICE_TREE`, `_FS`, `_GPIO`, `_ADC` | | |
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
| `EERIE_LEAP_CONFIGURATION` | | `THREADING` |
| `EERIE_LEAP_DOMAIN_SYSTEM` | `CONFIGURATION` | |
| `EERIE_LEAP_DOMAIN_ADC` | `ADC`, `CONFIGURATION` | |
| `EERIE_LEAP_DOMAIN_SENSOR` | `CONFIGURATION`, `ADC` | `DOMAIN_SCRIPT`, `DOMAIN_ADC` |
| `EERIE_LEAP_DOMAIN_CANBUS` | `CONFIGURATION`, `CANBUS` | |
| `EERIE_LEAP_DOMAIN_CANBUS_COM` | | `SMP_CAN` |
| `EERIE_LEAP_DOMAIN_LOGGING` | `CONFIGURATION` | |
| `EERIE_LEAP_DOMAIN_SCRIPT` | | |
| `EERIE_LEAP_DOMAIN_BLE` | `BLUETOOTH`, `DOMAIN_SENSOR`, `DOMAIN_SYSTEM` | `SMP_BLE` |
| `EERIE_LEAP_CONTROLLER_SYSTEM` | `DOMAIN_SYSTEM`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_MANAGEMENT` | `SMP`, `DOMAIN_SYSTEM` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_ADC` | `DOMAIN_ADC`, `DEVICE_TREE`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_CANBUS` | `DOMAIN_CANBUS`, `DOMAIN_CANBUS_COM`, `DEVICE_TREE`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_SENSORS` | `DOMAIN_SENSOR`, `DOMAIN_CANBUS`, `GPIO`, `RANDOM`, `FS` | `THREADING` |
| `EERIE_LEAP_CONTROLLER_BLE` | `DOMAIN_BLE` | |

The sensor domain needs `EERIE_LEAP_ADC` even on a unit without an ADC: the subsystem then
provides a simulator, and the sensors controller accepts a null ADC manager.

Log levels: `EERIE_LEAP_RT_CORE_LOG_LEVEL_*` for the module, `EERIE_LEAP_CANBUS_LOG_LEVEL_*`
and `EERIE_LEAP_LUA_LOG_LEVEL_*` for those two.

## Boot sequence

The order rt_daq's `main()` uses; dependencies flow downwards.

1. `DtConfigurator::Initialize(features)` reads the device tree.
2. Display (`Cfb`) and the application's display controller.
3. Filesystems: the SD service if a card slot exists, then the internal filesystem (fatal if it
   fails).
4. `TimeService`.
5. The **configuration work queue** (`WorkQueueThread`, 6144 bytes, priority 5): every
   configuration load, save, SMP apply and service restart runs on it.
6. `SystemController`, then `ConfigurationService` with the system manager registered.
7. `AdcController` (fatal if the ADC fails to initialize).
8. GPIO through `GpioFactory`.
9. `CanbusController`: initialize, then `Start()` the COM service.
10. The shared `SensorReadingsFrame` and `SensorsController`. Register the processing service
    with the ADC controller (`RegisterPausedService`) and the CAN controller
    (`RegisterDependentService`).
11. The application's logging controller when an SD card exists.
12. `ManagementController`, after every manager is registered.
13. `CanbusSchedulerService`, registered as a CAN dependent service.
14. `sensors_controller->Start()`, `canbus_scheduler_service->Start()`.

A BLE unit creates `BleController` after the sensors controller and starts it last.

## Tests

`west twister -T ./tests` runs the unit and functional suites on native_sim and QEMU (see the
top-level `README.md` for platforms and known gaps). The functional suites under
`tests/functional/domain/` are the executable reference for the behaviours described here.

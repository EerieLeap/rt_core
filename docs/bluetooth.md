# Bluetooth access point

A unit with Bluetooth advertises one GATT service through which a central (the EerieLeap app)
manages this unit and, over the CAN network, every other unit, and streams live sensor values.

Code: `src/subsys/bluetooth/` (stack wrapper, GATT service), `src/subsys/smp/ble/` (SMP
router), `src/domain/ble_domain/` (service, live data, `live` SMP group).

## GATT service

Base UUID `e7a1b2c3-d4e5-6f78-9a0b-c1d2e3f4xxxx`:

| Characteristic | UUID suffix | Properties | Content |
| --- | --- | --- | --- |
| service | `0000` | | Advertised in the 128-bit UUID list |
| `info` | `0001` | read, no pairing needed | `[protocol version][max packet size: u16 LE][requests in flight]` |
| `smp` | `0002` | write and write without response (encrypted link required), notify | SMP packets with a route prefix |
| `live` | `0003` | notify | Live value notifications |
| `bus` | `0004` | notify | The CAN monitor's stream |

`bus` is appended after `info` so that no existing handle moves, and its client configuration
descriptor is open like `live`'s. A bonded central that cached the table learns of it through
Service Changed, which needs `CONFIG_BT_GATT_SERVICE_CHANGED`. The characteristic is always
present; whether anything streams on it depends on the CAN monitor.

**Advertising.** The advertisement carries the flags, the service UUID and manufacturer data
(7 bytes: `company 0xFFFF`, product ID, feature bits, revision, little-endian, from the system
domain's Kconfig; there is no product family byte); the scan response carries `CONFIG_BT_DEVICE_NAME`. One connection at a time; advertising
restarts after a disconnect. A client can read `info` and subscribe before pairing, but SMP
writes are refused until the link is encrypted.

**Connection parameters.** While SMP writes arrive the service asks for a short connection
interval (7.5-15 ms) and the 2M PHY; after 3 s without SMP traffic it asks for a long interval
(30-50 ms) again. A stream makes no SMP writes, so the access point also gives out holds:
`HoldFast()` takes one and `ReleaseFast()` gives it back, and each call needs one release. Holds
are counted. While any is held the short interval and the 2M PHY are requested at once (on the
first hold, and again on a new connection) and SMP traffic does not start the 3 s timer; the
timer starts when the last hold is released. The CAN monitor holds from the opening of a session
to its end.

**The 15-30 ms fallback.** Apple's accessory guidelines ask for an interval of 15 ms or more, so
an iOS central may refuse the 7.5 ms minimum. When the update that answers a short request still
gives an interval above 15 ms, the service asks once (once per short request) for 15-30 ms and
logs the interval it got. The decisions are `FastLinkPolicy`, a header with no Bluetooth in it,
unit tested in `tests/unit/subsys/bluetooth`.

**Pairing** pauses the sensor processing service and resumes it when pairing finishes, so the
crypto work does not disturb sampling.

## SMP over BLE

Every packet on `smp` is `[route][SMP packet]` and may span several writes or notifications; the
SMP header's length says where it ends. The route is the destination of a request: 0 or this
unit's own CDMP address reaches the local SMP server, another reachable CDMP address is forwarded
over CAN through the `ISmpForwarder` (the COM service), anything else is answered with
`MGMT_ERR_ENOENT`. A response carries the route of the unit that answered. Dropped packets
(malformed, oversized, no buffer, unreachable) are counted.

`SmpBleRouter::IsNotifying()` is true from the moment a response is queued until its last
fragment has been handed to the stack (or it was dropped). The CAN monitor's sender waits while it
is true, so an SMP answer never waits behind the stream.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_SMP_BLE` | n | The router; selects `EERIE_LEAP_SMP`. |
| `EERIE_LEAP_SMP_BLE_RX_TIMEOUT_MS` | 1000 | Gap between writes after which a partial packet is dropped. |
| `EERIE_LEAP_SMP_BLE_MAX_FIRST_FRAGMENT_SIZE` | 20..512, 244 | Buffer for the first notification of a packet, which carries the route byte; 244 fills an ATT MTU of 247. |
| `EERIE_LEAP_SMP_BLE_REQUESTS_IN_FLIGHT` | 1..16, 2 | Reported in `info`; must stay below the shared SMP packet pool. |

## Live data

A central subscribes with the `live` SMP group (`SUBSCRIBE {ids, period, fields?}`, see
[smp.md](smp.md)) after enabling notifications on `live`. `LiveDataService` then samples the
latest numbers of the subscribed entries every `period` ms (at least 50) on the system work queue
and notifies:

```
[version=1][seq][t_ms: u32 LE][n] then n × [index][value: f32 LE]
```

`index` is the entry's position in the subscription; entries without a number are left out. An
entry is a sensor and a field (`fields` in `SUBSCRIBE`: 0 value, 1 calibrated voltage, 2 ADC
voltage; the voltages exist for analog sensors only), and a sensor may be listed once per field. Up
to 47 entries fit one notification at an MTU of 247, which counts entries, not sensors: 15 analog
sensors with all three fields take 45 of them. At most one notification is in flight: a
sample that finds one pending is dropped, and the sequence number counts samples, so a gap is
visible to the client. The subscription ends on `UNSUBSCRIBE` or disconnect.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_BLUETOOTH` | n | Stack wrapper and GATT service. Depends on `CONFIG_BT`; selects `EERIE_LEAP_THREADING`. |
| `EERIE_LEAP_DOMAIN_BLE` | n | `BleService` and live data. Depends on `EERIE_LEAP_BLUETOOTH`, `EERIE_LEAP_DOMAIN_SENSOR`, `EERIE_LEAP_DOMAIN_SYSTEM`; selects `EERIE_LEAP_SMP_BLE`. |
| `EERIE_LEAP_CONTROLLER_BLE` | n | `BleController`. Depends on `EERIE_LEAP_DOMAIN_BLE`. |

Zephyr's own Bluetooth options (`CONFIG_BT_DEVICE_NAME`, security, MTU, PHY) apply as usual.

## Wiring

`BleController(sensors_processing_service, sensor_readings_frame, smp_forwarder)` creates the
single `BleService`, initializes the stack, the access point, the SMP router and the live data
service, and `Start()` begins advertising. Pass the CAN COM service as the forwarder to reach
other units, or `nullptr` on a unit that only serves itself.

For the CAN monitor `BleService` hands out `GetBusNotifier()`, `HoldFast()` and `ReleaseFast()`,
`IsSmpNotifying()` and `SetBusSentHandler()`, which replaces the handler of `bus` notification
completions (it runs on the system work queue; the call waits for a running handler to return).

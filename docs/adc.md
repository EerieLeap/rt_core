# ADC

Analog inputs in two layers: the **ADC subsystem** (`src/subsys/adc/`) knows the hardware and
converts; the **ADC domain** (`src/domain/adc_domain/`) holds the user's configuration (how
many conversions to average and a calibration per channel) and the calibration service. Sensors
of type `PHYSICAL_ANALOG` read through both; see [sensors.md](sensors.md).

## Hardware

**Device tree.** One ADC per `adc0` … `adc3` alias, with `channel@N` children; properties are
listed in [device-tree.md](device-tree.md). Channels are numbered globally: ADCs in alias
order, channels in child order. That global index is what sensors and the calibration client use.

**Factory.** `AdcFactory(DtAdc::Get).Create()` builds one `IAdc` per device tree ADC:
`AdcEmulator` under `CONFIG_ADC_EMUL` (a random stimulus is written to the emulated channel
before every read), otherwise `Adc`. Without any ADC in the device tree an `AdcSimulator` with
8 channels of random volts stands in, so the rest of the system runs unchanged.

**Initialize.** Checks the device, rejects an empty channel list and a channel with resolution 0,
sets every channel up, then probes the driver: one timed single-channel read gives the
microseconds per conversion, a read with `extra_samplings` tells whether bursts are accepted, a
two-channel mask tells whether multi-channel scans are. The result is logged once per ADC:

```
adc0: 4 channels, burst=yes multi=no oversampling=0, ~25 us/conversion
```

**Sample.** `IAdc::Sample(request)` takes a list of local channels, a result slot per channel and
`samples`, the number of conversions to average. It never throws or allocates; every result
carries its own status:

- `samples` is clamped to `EERIE_LEAP_ADC_SAMPLES_MAX`. Conversions are read in chunks of
  `EERIE_LEAP_ADC_BURST_CHUNK` through one driver call where the driver supports bursts and
  scans, otherwise channel by channel or conversion by conversion.
- `-ETIMEDOUT` and `-EBUSY` are retried twice. Any other error, or a third timeout, marks the
  channel (`AdcError::DRIVER` or `TIMEOUT` plus the errno) and the next channel is sampled.
- `volts` is the mean of the conversions in ADC-pin volts: raw sum × (vref × gain⁻¹ / 2^res) /
  samples, computed once per channel.
- `GetStats()` counts requests, driver calls, conversions, errors and timeouts.

`IAdcManager` holds the ADCs in alias order, reports the device tree channel count and resolves
a global channel index to `{adc, local channel}` (`Resolve()`, construction time only; it throws
for an unknown channel).

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_ADC` | n | Builds the subsystem. Real hardware needs `CONFIG_ADC`; QEMU and native_sim use `CONFIG_ADC_EMUL`. |
| `EERIE_LEAP_ADC_SAMPLES_MAX` | 1..65535, 4096 | Cap on `samples` per request; larger requests are clamped. Every conversion blocks the sampling thread, so size it for the slowest driver. |
| `EERIE_LEAP_ADC_BURST_CHUNK` | 1..1024, 64 | Conversions per driver call on a bursting driver. Each ADC keeps `channels × chunk` 16-bit samples in internal RAM. |

## Voltage path

```
ADC pin volts ──AdcCalibrator──► calibrated sensor-input volts ──sensor table──► value
   (Adc)          (ADC domain)                                   (sensor domain)
```

`AdcRange` describes the input divider: `EERIE_LEAP_ADC_VOLTAGE_MAX_MV` at the ADC pin
corresponds to `EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV` at the sensor input (defaults 3.1 V and
5 V). Voltages beyond either end saturate.

`AdcCalibrator` holds one channel's calibration: an interpolation method and a table of points
`{voltage, value}` where `voltage` is the true voltage at the sensor input and `value` what the
unit read for it, both in sensor volts. The calibrator keys the table by the reading (converted back to ADC-pin volts through the
divider) and interpolates to the true voltage: `InterpolateToCalibratedRange(adc_volts)` clamps
the reading to the ADC range and returns the calibrated sensor-input voltage.
`InterpolateToInputRange()` applies the divider only, which is what the calibration service
reports.

## Stored configuration

Schema (`adc_config.cddl`):

```
CborAdcCalibrationDataMap = { 2*50(float32 => float32) }     ; voltage => value
CborAdcChannelConfig = [ interpolation_method: uint, ? calibration_table: CborAdcCalibrationDataMap ]
CborAdcConfig = [ samples: uint, adc_channels_config: [*CborAdcChannelConfig] ]
```

| Field | Meaning |
| --- | --- |
| `samples` | Conversions averaged per channel on every sampling tick, 1..`EERIE_LEAP_ADC_SAMPLES_MAX`. |
| `adc_channels_config[i]` | Calibration of global channel `i`. `interpolation_method`: 1 `LINEAR`, 2 `CUBIC_SPLINE`; the table is required. |

Validation (on load, on `Update()`, on an SMP write):

- `samples` in range, checked on the stored width before narrowing.
- At most as many channels as the device tree has. Fewer are allowed: the missing channels get
  the default calibration and a warning, so a firmware update that adds channels keeps the
  existing calibration.
- Every channel has a method and a table of 2 to 50 finite points, strictly increasing in both
  columns, whose values map to distinct ADC voltages (values beyond the sensor range saturate and
  would collide). A stored table is sorted by voltage when loaded, so an imported table may be
  unordered.

Default (no file, or a file that fails to load): `samples = min(40, SAMPLES_MAX)` and a linear
six-point near-identity table on every channel.

**Taking effect.** A stored configuration never touches the hardware. The sensors controller
registers an update handler on the ADC manager and restarts the sensor processing service, whose
new generation snapshots the ADC configuration and rebuilds the analog readers with the new
calibrators. The generation that is running keeps the calibrators it was built with.

| Symbol | Range / default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_ADC` | n | Builds the domain. Depends on `EERIE_LEAP_ADC` and `EERIE_LEAP_CONFIGURATION`. |
| `EERIE_LEAP_ADC_VOLTAGE_MAX_MV` | 1..100000, 3100 | ADC pin voltage at full sensor input. |
| `EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV` | 1..100000, 5000 | Sensor input voltage at the end of the range. |
| `EERIE_LEAP_ADC_CALIBRATION_SAMPLES_COUNT` | 1..65535, 2048 | Conversions averaged per calibration reading, at most `SAMPLES_MAX`. |
| `EERIE_LEAP_ADC_CALIBRATION_SAMPLING_RATE_MS` | 100 | Period of calibration readings. |

## Calibration service

`CalibrationService` samples one channel on its own thread (stack 4 KiB, priority 6) every
`CALIBRATION_SAMPLING_RATE_MS` with `CALIBRATION_SAMPLES_COUNT` conversions, while the service
that normally owns the ADC (the sensor processing service) is paused. One job runs at a time; the
latest result is kept for the client:

| Result field | Meaning |
| --- | --- |
| `adc_volts` | ADC-pin volts, uncalibrated. |
| `sensor_volts` | The same through the divider only, no table applied. |
| `timestamp`, `sequence` | Time of the sample; sequence increases with every result. |
| `error` | `AdcError` of the sample. |

`Start(channel)` returns `false` while a job runs, for a channel the unit does not have, or when
the service to pause cannot be paused (another owner has it). `Stop()` cancels the job, waits for
it and resumes what it paused, unless its owner restarted or stopped it meanwhile.

With `CONFIG_EERIE_LEAP_SMP` the service is served as the SMP `calibration` group (`START`,
`STOP`, `READ`; see [smp.md](smp.md)). A client takes a reading of a known input voltage per
point, builds the table from the `sensor_v` values, and writes the ADC configuration back
through the `config` group.

## Wiring

`AdcController` (`EERIE_LEAP_CONTROLLER_ADC`; depends on `EERIE_LEAP_DOMAIN_ADC`,
`EERIE_LEAP_DEVICE_TREE`, `EERIE_LEAP_FS`):

1. Creates the ADC manager from the device tree and initializes it. A failure is fatal.
2. Creates the configuration manager on `config/adc_config.cbor` and registers it with
   `ConfigurationService` as type 2.
3. With a time service and at least one channel, creates the calibration service and the SMP
   group.
4. `RegisterPausedService(service)` hands it the service to pause during calibration. The SMP
   group is only registered once this has been called (with `nullptr` on a unit that has nothing
   to pause), so no client can calibrate under the sensors' own sampling.

The application passes the manager and the configuration manager on to the sensors controller;
without them `PHYSICAL_ANALOG` sensors get no reader.

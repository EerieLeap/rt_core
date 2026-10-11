# Device tree

rt_core finds its hardware through a fixed set of device tree aliases and `chosen` properties.
The accessors in `src/subsys/device_tree/` read them once, at boot, when the application calls
`DtConfigurator::Initialize()` with the features it uses:

```cpp
DtConfigurator::Initialize(
    DtFeature::INTERNAL_FS | DtFeature::SD_FS | DtFeature::GPIO
    | DtFeature::ADC | DtFeature::DISPLAY | DtFeature::CANBUS);
```

A feature whose nodes are absent from the device tree is simply reported as missing (`nullptr`
or an empty optional); the factories then fall back to a simulator where one exists.

Kconfig: `EERIE_LEAP_DEVICE_TREE` builds the accessors. It has no options.

## Aliases and chosen nodes

| Node | Accessor | Used by | Notes |
| --- | --- | --- | --- |
| alias `intfs0` | `DtFs::GetInternalFsMp()` | Configuration storage | An `fstab` entry (littlefs or FAT). The internal filesystem holds `config/`. |
| alias `sdfs0` + chosen `zephyr,sdhc` | `DtFs::GetSdFsMp()`, `GetSdDiskName()` | Logging, scripts | An `fstab` entry with `disk-access`; the SDHC node's child disk node provides `disk-name`. Needs `CONFIG_SDMMC_SUBSYS`. |
| alias `gpioc` | `DtGpio::Get()` | GPIO subsystem | A `gpio-keys` node. Each child with a `gpios` property is one input channel, numbered in child order from 0. |
| aliases `adc0` … `adc3` | `DtAdc::Get()` | ADC subsystem | Each is one ADC controller with `channel@N` children. Channels are numbered globally: ADCs in alias order, channels in child order. |
| aliases `can0`, `can1` | `DtCanbus::Get(bus_channel)` | CAN subsystem | Bus channel 0 and 1 of the stored CAN configuration. Needs `CONFIG_CAN`. |
| chosen `zephyr,display` | `DtDisplay::Get()` | CFB | Needs `CONFIG_DISPLAY`. |

## ADC channel properties

For each `channel@N` child the standard Zephyr ADC channel bindings are read with
`ADC_CHANNEL_CFG_DT`, plus:

| Property | Required | Meaning |
| --- | --- | --- |
| `zephyr,gain`, `zephyr,reference`, `zephyr,acquisition-time` | yes | Driver channel setup. |
| `zephyr,vref-mv` | yes | Reference voltage; with `ADC_REF_INTERNAL` the driver's internal reference is used. |
| `zephyr,resolution` | yes | Bits. A channel with resolution 0 makes ADC initialization fail. |
| `zephyr,oversampling` | no | Passed to the driver as the oversampling of every read. A driver that rejects it (`-ENOTSUP`) fails initialization: the overlay states a fact the hardware cannot honour. |

Channels of one ADC that differ in resolution or oversampling disable multi-channel scans on that
ADC (each channel is then read on its own).

## Example overlay

A trimmed version of the `native_sim` overlay used by the application:

```dts
/ {
    aliases {
        intfs0 = &intfs0;
        gpioc = &gpioc;
        adc0 = &adc_emul;
        can0 = &can_loopback0;
    };

    fstab {
        compatible = "zephyr,fstab";
        intfs0: intfs0 {
            compatible = "zephyr,fstab,littlefs";
            partition = <&intfs0_partition>;
            mount-point = "/intfs0";
            automount;
        };
    };

    gpioc: gpioc {
        compatible = "gpio-keys";
        di_0: di_0 { gpios = <&gpio0 9 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>; };
        di_1: di_1 { gpios = <&gpio0 10 (GPIO_PULL_UP | GPIO_ACTIVE_LOW)>; };
    };
};

&adc0 {
    #address-cells = <1>;
    #size-cells = <0>;

    channel@0 {
        reg = <0>;
        zephyr,gain = "ADC_GAIN_1";
        zephyr,reference = "ADC_REF_INTERNAL";
        zephyr,vref-mv = <3300>;
        zephyr,acquisition-time = <ADC_ACQ_TIME_DEFAULT>;
        zephyr,resolution = <12>;
    };
};
```

On the ESP32-S3 processing module board the same aliases map to the two internal ADCs
(`adc0`, `adc1`), the TWAI controller (`can0`) and an external MCP2518FD (`can1`), an SPI SDHC
slot (`zephyr,sdhc`, disk name `SD`) and an SSD1306 display (`zephyr,display`).

## Simulated and emulated hardware

| Hardware | With | Without any device |
| --- | --- | --- |
| ADC | `CONFIG_ADC_EMUL`: emulated ADCs driven with random values | `AdcSimulator`: 8 channels of random volts |
| GPIO | `CONFIG_GPIO_EMUL`: emulated inputs | `GpioSimulator` |
| CAN | `can_loopback0` or a real controller | A configured bus whose device is missing is logged and skipped |
| Flash | `CONFIG_FLASH_SIMULATOR` | |

The test overlays in `tests/` (`qemu_adc.overlay`, `qemu_flash.overlay`,
`qemu_flash_disk.overlay`) are the reference for QEMU targets.

# GPIO inputs

Digital input channels for `PHYSICAL_INDICATOR` sensors. Code: `src/subsys/gpio/`.

## How it works

- Channels come from the `gpioc` alias, a `gpio-keys` node: child `n` is channel `n`. The
  `gpios` flags (`GPIO_ACTIVE_LOW`, pull-ups) are applied by the driver, so `ReadChannel()`
  returns the logical level.
- `GpioFactory(DtGpio::Get).Create()` returns `GpioEmulator` under `CONFIG_GPIO_EMUL`, `Gpio`
  under `CONFIG_GPIO`, and `GpioSimulator` when no `gpioc` node exists.
- `RegisterChannelChangedHandler(channel, edge, handler)` attaches an interrupt on the first
  registration of a channel. The interrupt only latches the level and submits a work item to the
  system work queue, where every handler of the channel runs in thread context. Edges closer than
  the debounce time to the previous accepted edge are ignored. `GpioEdge` selects `ACTIVE`,
  `INACTIVE` or `BOTH`.
- Handlers share the system work queue: anything long-running must hop to its own queue, as the
  sensor domain's `GpioSensorReader` does.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_GPIO` | n | Builds the subsystem. Needs `CONFIG_GPIO` for real hardware. |
| `EERIE_LEAP_GPIO_DEBOUNCE_MS` | 50 | Minimum time between two accepted edges on one channel. |

## Use by sensors

A `PHYSICAL_INDICATOR` sensor names a channel. Without a sampling rate it is updated on every
edge (both directions), coalesced to the newest level if edges arrive faster than the processing
queue drains, and seeded with the current level when the pipeline starts. With a sampling rate it
is polled like any other sensor. See [sensors.md](sensors.md).

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
  system work queue, where every handler of the channel runs in thread context. While the work
  item is queued, further edges only update the latched level, so a burst of edges costs one
  handler run, with the newest level. `GpioEdge` selects `ACTIVE`, `INACTIVE` or `BOTH`.
- The subsystem does not debounce. No time window drops edges. Edges are coalesced to the newest
  level, so a sensor always ends on the real level, and its `debounce_ms` settles a bouncing
  contact (see [sensors.md](sensors.md#indicator-conditioning)). A handler registered directly
  gets no debounce and runs on bounces.
- Handlers share the system work queue: anything long-running must hop to its own queue, as the
  sensor domain's `GpioSensorReader` does.

## Random channels

A bench and development aid, off by default. With `CONFIG_EERIE_LEAP_GPIO_RANDOMIZER` the factory
adds a `GpioRandomizer`: alone when the unit has no `gpioc` node (instead of the `GpioSimulator`,
which raises no edges), otherwise together with the hardware in a `CompositeGpio`, whose channels
are the hardware's followed by the random ones, so hardware keeps its numbers. A
`PHYSICAL_INDICATOR` sensor connects to a random channel by its global number.

A channel holds a level for a random time (`GpioRandomSignal`) and flips. Handlers follow the
contract above: ids, edge filters, `ERR_NOT_INITIALIZED` before `Initialize()`, the system work
queue, the newest level when the queue is late. A flip is scheduled only while a channel has
handlers, so an unwatched channel costs nothing; `ReadChannel()` gives the level now. The
composite hands out its own handler ids and calls a handler with the global channel.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_GPIO` | n | Builds the subsystem. Needs `CONFIG_GPIO` for real hardware. |
| `EERIE_LEAP_GPIO_RANDOMIZER` | n | Adds the random channels. |
| `EERIE_LEAP_GPIO_RANDOMIZER_CHANNELS` | 1..32, 4 | Number of random channels. |
| `EERIE_LEAP_GPIO_RANDOMIZER_HOLD_MIN_MS` / `_HOLD_MAX_MS` | 100 / 5000 | Bounds of a level's hold time. |

## Use by sensors

A `PHYSICAL_INDICATOR` sensor names a channel. Without a sampling rate it is updated on every
edge (both directions), coalesced to the newest level if edges arrive faster than the processing
queue drains, and seeded with the current level when the pipeline starts. With a sampling rate it
is polled like any other sensor. No time window drops edges, so the sensor ends on the real level;
its `debounce_ms` settles a bouncing contact, and `input_mode` `Toggle` makes a momentary button a
switch. See [sensors.md](sensors.md).

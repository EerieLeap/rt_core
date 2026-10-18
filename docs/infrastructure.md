# Infrastructure subsystems

The small subsystems every domain builds on: services and threads, time, the event bus, random
numbers, the character framebuffer. Each is enabled by one Kconfig symbol and has no further
options unless listed.

## Threading (`EERIE_LEAP_THREADING`)

Code: `src/subsys/threading/`. Selected by nearly everything.

**Services.** `IService` is the lifecycle every long-running component implements: `Initialize`,
`Start`, `Stop`, and optionally `Pause`/`Resume` (`IsPausable()`). `ServiceBase<>` owns the
state machine (`STOPPED`, `STARTING`, `RUNNING`, `PAUSED`, `STOPPING`); implementations only
provide `DoInitialize`, `DoStart`, `DoStop`, `DoPause`, `DoResume`. Rules:

- Transitions are serialized by a recursive mutex, so a `Pause()` arriving during a restart waits
  for it. Never call a transition from a thread the `Do*` work waits for.
- A `Do*` that returns `false` or throws leaves the state it found; the exception reaches the
  caller.
- `Start()` on a running service and `Stop()` on a stopped one return `true` without doing
  anything.

**Threads.** `Thread` wraps a Zephyr thread around an `IThread::ThreadEntry()`; stacks come
from the external heap when a memory resource is given. `WorkQueueThread` wraps a `k_work_q`;
`CreateTask()` returns a `WorkQueueTask<T>` whose handler is called with the task's user data and
returns `{reschedule, delay}` to run periodically. `Run()` queues a one-off callable.

## Time (`EERIE_LEAP_TIME`)

Code: `src/subsys/time/`. `TimeService` combines an RTC provider (wall clock) and a boot-elapsed
provider: `GetCurrentTime()` and `GetTimeSinceBoot()` return `std::chrono::system_clock`
time points at the kernel tick's resolution (`SYS_CLOCK_TICKS_PER_SEC`; 100 µs at the 10 kHz the
ESP32-S3 boards run). The RTC provider is not backed by hardware yet: it adds the uptime to a
fixed epoch (2025-10-22T04:10:17Z), so wall-clock dates in file names and log headers are only
relative. Every reading, log record and calibration result is stamped through `ITimeService`,
so a test can inject its own clock.

## Event bus (`EERIE_LEAP_EVENT_BUS`)

Code: `src/subsys/event_bus/`. A typed publish/subscribe transport:

- An `EventChannel<EventType, PayloadType>` owns subscribers and a fixed ring of queued events.
  Payloads are small inline maps (up to 6 keys) of `int`, `uint32_t`, `float`, `std::string` or
  `bool`.
- An `EventBus` owns one cooperative worker thread (default priority 10) and drains the channels
  registered with it round-robin, one event at a time. A channel not registered with a bus drops
  publishes but still accepts subscriptions.
- `PublishAsync()` queues; `Publish()` dispatches on the caller's thread. Filters run before the
  handler so unwanted events cost nothing. `SubscribeErased()` lets configuration-driven code
  subscribe without knowing the enums.
- `Accepts(event)` (typed) and `Accepts(event_type, source_id, payload)` (erased) ask whether a
  subscriber of that event type would take the event: they run the filters under the subscriber
  lock, queue nothing and call no handler. Because `Accepts()` runs them, filters also run on the
  asking thread, and more than once per event, so they must be pure; binding selectors are. False
  when nobody subscribes to the type; a subscriber without a filter takes everything. The bus is
  not consulted, so an inert channel can accept what it would drop. A filter that throws
  propagates, as in dispatch.
- `GetSubscriptionGeneration()` moves on every subscribe and unsubscribe, inside the subscriber
  lock with release order. A publisher that caches `Accepts()` keys its answers by it:
  `AcceptanceCache<N>` (`acceptance_cache.h`, header only) holds up to N answers by key (a sensor
  id hash) and forgets them all when the generation moves, or when it is full and a new key
  arrives. Read the generation after taking the events to judge, never before, so a change that
  happened before they were taken is seen (see `SensorEventsPublisher`).

**Bindings.** Stored configuration names an event by value: a channel id and the raw values of
the channel's enums, so it resolves without the code knowing those enums. Widget bindings
(rt_gauge) and logging triggers ([logging.md](logging.md#triggers)) use this.

- `EventBinding` (`event_binding.h`) holds the channel id, the event type, the payload key and an
  optional selector (`selector_key`, `selector_value`) that narrows the events to one sensor or one
  setting.
- `EventChannelId` (`event_channel_id.h`) names the channel. The ids are persisted and append only;
  one enum serves every application, so a stored id names the same channel on every unit.

  | Id | Value | Channel |
  | --- | --- | --- |
  | `None` | 0 | None; a binding to it is inert |
  | `Sensors` | 1 | `sensor_events` |
  | `Logging` | 2 | `logging_events` |
  | `Navigation` | 3 | `navigation_events`, rt_gauge only |
  | `Settings` | 4 | `settings_events`, rt_gauge only |

- `EventChannelRegistry` (`event_channel_registry.h`) maps an id onto the channel. Each
  application registers the channels it carries while it composes, before anything resolves a
  binding; `Find()` takes no lock. `Find()` returns null for an id nobody registered, so a stale
  binding is inert. `Register()` refuses `None` and any id `IsValidEventChannelId()` does not name
  (`-EINVAL`), and a second channel under a taken id (`-EEXIST`). Appending an id means moving
  `LAST_EVENT_CHANNEL_ID` to it; `IsValidEventChannelId()` and the registry's table follow.
- A selector is text, an integer or a bool (`IsSelectorValue()`). Text is compared by its hash,
  because payloads carry ids hashed (`ToSelectorValue()`, `SelectorMatches()`). An integer matches
  an `int` or `uint32_t` entry, a bool a `bool` entry.
- `SubscribeEventBinding(binding, handler)` (`event_binding_subscription.h`) resolves and
  subscribes in one step. The selector becomes the channel filter, run under the subscriber lock.
  The handler receives the value under the payload key, on the dispatching thread. It returns null
  for `None`, an unregistered channel, a selector that cannot select, an empty handler or a refused
  subscription; the caller logs it.
- `DispatchGuard` (`dispatch_guard.h`). Dispatch snapshots the matching handlers under the
  subscriber lock and calls them outside it, so destroying a subscription does not stop a delivery
  already under way. A handler that can outlive its owner holds a shared guard and runs its body
  through `Dispatch()`. The owner calls `Detach()` before it tears down; it waits for a delivery
  under way on another thread and makes later ones no-ops. The lock is recursive, so never call
  `Detach()` from inside the guard's own `Dispatch()` body (it would not wait), nor while holding a
  lock a body takes inside the guard (it would deadlock). Detaching from another subscriber's
  handler is safe, and so is holding a lock the handlers take before `Dispatch()`: UI owners take
  the LVGL lock, then the guard, and detach under the LVGL lock.
- `ConfigValue` (`src/utilities/type/config_value.h`) is the loosely typed value of stored
  configuration, and the type of a selector.

`EventBinding`, `IsSelectorValue()` and `IsValidEventChannelId()` are header only, so
configuration parsers and validators use them in builds without `EERIE_LEAP_EVENT_BUS`.

Channels in the tree: `sensor_events` (`SensorEventType::DataUpdated`, payload `SensorId`,
`Value`; 128 queued events, on the bus the composition root registers it with: rt_core's
`SensorEventBus` (4 KiB stack, this channel only) on rt_gauge, the application bus that also
carries `logging_events` on rt_daq, see [logging.md](logging.md#wiring); published by
`SensorEventsPublisher`, see [sensors.md](sensors.md#sensor-events)) and `logging_events`
(`LoggingEventType::StatusUpdated`, payload `IsActive` and, when inactive, `Reason`; 8 queued
events, on the bus the composition root registers it with; see
[logging.md](logging.md#status-events-and-failures), and
[logging.md](logging.md#status-on-other-units) for the status of other units on it). A channel stays
with the first bus that registers it (`RegisterChannel()` returns `-EEXIST` on another), so a unit
that puts `sensor_events` on its own bus does not create `SensorEventBus`.

## Random (`EERIE_LEAP_RANDOM`)

`Rng::Get<T>(secure)` fills an unsigned integer from Zephyr's random subsystem; `secure` uses
the CSPRNG. Used for the device ID, CDMP UID fallback, log file names and SMP tokens. Needs an
entropy source (`CONFIG_ENTROPY_GENERATOR` or `CONFIG_TEST_RANDOM_GENERATOR` on QEMU).

## Character framebuffer (`EERIE_LEAP_CFB`)

Code: `src/subsys/cfb/`. `Cfb` wraps Zephyr's CFB on the `zephyr,display` device: text, lines,
rectangles, circles, area inversion, `Flush()`/`Clear()`, and an animation handler run from its
own work queue at a frame rate (stack 2 KiB, priority 6). The 8x8 font is built in. Needs
`CONFIG_CHARACTER_FRAMEBUFFER` and `CONFIG_DISPLAY`.

## Memory

Models that live for the length of a configuration (sensors, CAN messages, readings frames) are
allocated through `std::pmr` on the external heap (`Mrm::GetExtPmr()`, PSRAM on ESP32-S3). Hot
paths (a sampling tick, a CAN frame, a log record) allocate nothing: buffers are sized when a
configuration generation is built.

## Thread and work queue priorities

Lower is more urgent. Defaults as compiled in rt_core; the application may override the Kconfig
ones.

| Thread | Priority | Stack | Set by |
| --- | --- | --- | --- |
| CAN RX bottom half (per bus) | highest, cooperative | 2048 | fixed; give it a MetaIRQ level with `CONFIG_NUM_METAIRQ_PRIORITIES` |
| SD card monitor | 2 | `EERIE_LEAP_FS_SD_THREAD_STACK_SIZE` | fixed |
| CDMP service thread and work queue | 5 | 4096 | `EERIE_LEAP_CDMP_*` |
| Configuration work queue | application | application | the application creates it (8192 / 5 in rt_daq); log writer starts and stops, trigger rebuilds, the remote `LOGGING` send and the `LOGGING` stop of a configuration update run on it; so does the report of an update stored by an SMP write ([logging.md](logging.md#supervision), [logging.md](logging.md#configuration-updates), [logging.md](logging.md#threading)), and every request of the SMP `assets` group, file I/O and CRC included, while the MCUmgr handler waits (a resumed download first reads the file up to its offset; [smp.md](smp.md#assets-asset-kinds)) |
| Sensor processing | 6 | 8192 | `EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_*` (rt_daq uses 5) |
| Sensor events publisher | 6 | 4096 | `EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_*` |
| Calibration sampling | 6 | 4096 | fixed |
| CAN scheduler (frame sending) | 6 | 8192 | fixed |
| CFB animation | 6 | 2048 | fixed |
| MCUmgr (SMP) work queue | 7 | 4096 with the ADC controller, else MCUmgr default | `MCUMGR_TRANSPORT_WORKQUEUE_*` defaults set by `EERIE_LEAP_SMP` |
| Log writer | 8 | `EERIE_LEAP_LOG_WRITER_STACK_SIZE` | fixed priority |
| Event bus worker | 10, cooperative | per bus | fixed |

Timing on the processing module (from the application's notes): sensor processing one level
above the display, CAN scheduler and calibration threads removed most of the period jitter of
the 50 ms rate group.

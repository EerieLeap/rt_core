# Logging

The logging domain (`src/domain/logging_domain/`) writes sensor readings to a file on the SD
card at a fixed interval, in MDF 4.10 by default. The MDF writer itself is the MDF subsystem
(`src/subsys/mdf/`).

Logging control (`src/domain/logging_domain/control/`) starts and stops logging, on this unit or
on the units that log: from triggers stored in the configuration, from the CDMP `LOGGING` command
and from the application's shell. `LoggingController` (`EERIE_LEAP_CONTROLLER_LOGGING`) builds it
on units with and without a log writer (see [Control](#control)). A configuration stored on any
unit stops logging on every unit (see [Configuration updates](#configuration-updates)).

## How it works

`LogWriterService` is a service with its own work queue (priority 8, stack
`EERIE_LEAP_LOG_WRITER_STACK_SIZE`). `Start()`:

1. needs a logger factory, an available SD card, a `logging_interval_ms` of at least 10 and
   `EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` free on the card;
2. builds this session's logger with the factory (`SetLoggerFactory()`), so every start, a
   restart included, writes the layout of the configurations in effect at that moment;
3. creates the `logs/` directory (`LogFileNaming::LOGS_DIR`) if missing and picks a new file name
   `<prefix>_<YYYYMMDD>T<HHMMSS>Z_<rand4>.<ext>` from the start time in UTC and a random
   four-digit suffix (`log_data_20261011T093000Z_4242.mf4` by default), so names sort by start
   time; it tries up to ten suffixes when a name is taken (`LogFileNaming` builds the name).
   The directory is fixed, and is the directory of the `logs` asset kind (see
   [Log files as assets](#log-files-as-assets)); logs that earlier firmware wrote to `data_logs/`
   stay there and are not listed;
4. discards the raw CAN frames queued before the start (the frame queues them whether or not
   logging runs), then opens the file through a seekable stream and calls the logger's
   `StartLogging()`;
5. publishes `StatusUpdated` with `IsActive = true` (see below) and schedules the first tick one
   `logging_interval_ms` later.

Each tick snapshots every sensor's latest processed reading from the `SensorReadingsFrame`, or
its latest reading when that is an error (see Invalidation bits below), logs the ones the
configuration enables, then drains the raw CAN frame queue so every frame of a `CANBUS_RAW`
sensor is logged, not only the latest one. The tick passes its own time to the logger, which
writes every reading at the reading's own timestamp, so values keep their sample time and the
frames drained in one tick keep their arrival times; the tick time stands in for a reading without
a timestamp and for a held sample written again (`log_only_new_data` off). Neither the tick nor
the logger follows a reading's sensor pointer, which belongs to the generation of sensor
processing that produced it. The tick allocates nothing: its buffers are sized by
`EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT` and `_RAW_CAN_QUEUE_SIZE`. `Start()` writes and syncs the
file header, so a card that refuses writes fails the start instead of a later tick and no empty
file is left behind. Records collect in the stream's write buffer (see
[filesystem.md](filesystem.md)); at the end of a tick, once `EERIE_LEAP_LOG_SYNC_INTERVAL_MS`
has elapsed since the last sync, the tick flushes and syncs the file, so the size and allocation
on the card cover everything up to that point and a power loss costs at most one interval plus
one buffer. `Stop()` waits for a tick in progress, then finalizes and closes the file; finalizing
syncs twice, once after the lengths and counts are patched and once after the identifier flips,
so the identifier never claims a length the card does not hold yet. It publishes `IsActive =
false` with the reason `Stopped`.

### Deadlines and raw CAN capacity

Ticks run on absolute deadlines (`K_TIMEOUT_ABS_TICKS`, so `CONFIG_TIMEOUT_64BIT` is required),
each one interval after the previous one: neither the tick's own duration nor the kernel's
rounding of a relative timeout adds to the period. A tick that runs past the next deadline (a
slow sync, a rotation) keeps the phase; the deadlines it missed are skipped and counted as
`overruns` rather than run late, back to back.

Raw CAN frames wait in the frame's queue between two drains: `EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE`
entries (32 by default), about 90 bytes each, and the writer's drain buffers are sized the same.
A frame that arrives while the queue is full is dropped and counted in `frames_dropped`, and the
writer warns once per `EERIE_LEAP_LOG_STATS_LOG_PERIOD_S` while that count grows. When a drain
fills the buffers the queue may already be dropping, so the next tick comes a quarter interval
later (10 ms at the least) and the phase goes on from there. Size the queue for
`logging_interval_ms` x the peak frame rate of the `CANBUS_RAW` sensors: at 100 ms and 1000
frames/s, 100 entries; the shortened tick absorbs a burst, not a sustained rate above that.

### Rotation and free space

With `max_log_size_mb` above 0, the tick that brings the file to that size marks it for
rotation, and the next tick, before its snapshot, finalizes and closes it and opens the next
file. The next file starts at the time of the previous tick rather than of the rotation, so the
frames and samples that arrived meanwhile are not older than the file and keep their own times;
every raw frame is in exactly one of the two files. Rotations are counted in `rotations`, and the
stats' byte and write counts cover every file of the session.

`EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` (default 16, 0 disables the check) is checked when logging
starts and before each rotation, never per tick: the free space comes from FatFs's free cluster
count, which can take a scan of the whole FAT on a large card. It is the volume's total space minus
its used space, both counted in allocation units (a FAT cluster), so the minimum means the room it
names. A start without it fails; a rotation without it ends the session with the reason `NoSpace`,
leaving the full file open for the stop to finalize.

### Log files as assets

The files in `logs/` are the `logs` asset kind (see [filesystem.md](filesystem.md#assets-eerie_leap_assets)):
a client lists, downloads and deletes them with the SMP `assets` group (see [smp.md](smp.md)). The
kind takes no uploads: the writer is the only source of logs. It serves files ending in `.mf4`; a
name that `LogFileNaming` builds is `EERIE_LEAP_LOG_DATA_FILE_PREFIX` plus 26 characters
(`_YYYYMMDDTHHMMSSZ_NNNN.mf4`), which must be a valid asset name: a build with `EERIE_LEAP_ASSETS`
refuses a prefix that makes the longest name exceed `EERIE_LEAP_ASSETS_MAX_NAME_LENGTH`, holds a
character outside `A-Z a-z 0-9 . _ -` or starts with a dot.

The file the writer has open is in use: it can be neither downloaded nor deleted (`IN_USE`), where
every other log can. `LogWriterService::IsOpenFile(path)` says which that is, for a path relative to
the volume, compared ignoring ASCII case, from any thread; it takes a lock of its own that is never
held across file I/O. The writer sets the path before the file exists and clears it where the file is
closed and final (after the finalize), on every failure path and on a stop, so a stopped writer has no
open file and the log of the last session can be fetched, and a rotation has none between its two
files. Only a unit with a log writer registers the kind: `LoggingController` in the writer role, with
the SD service and a registry (see [Wiring](#wiring)); the remote role registers nothing.

**End times.** A finalized MDF4 header holds no end time, so the writer records it
(`LogEndTimes`, `services/log_end_times.h`): when it closes a file with a successful finalize, on a
stop or a rotation, it records the time of the file's last tick (the file's start for one closed before
its first tick), and a file that could not be finalized gets none. The index lives in memory and in
`logs/end_times.txt` (`LogFileNaming::END_TIMES_FILE_NAME`), a line `<end_ms> <file name>` per log,
`end_ms` in Unix milliseconds, UTC. It ends in neither log extension, so the `logs` kind never serves
it. A record appends a line; the file is rewritten from memory once its lines outnumber the cap twice.
It is read once per mount, when `LoggingController` wires the writer and at a `Start()` after the card
came back: unreadable lines and the entries of logs that are gone are dropped and the file is rewritten
pruned (a missing or corrupt file is an empty index, with a warning); a deleted log's entry is ignored
until then, since nothing lists it. Logs are never renamed, so a name keyed entry stays right. At most
256 entries are kept, the oldest recorded leaving first (a log older than that lists without an end).
An index failure is logged and never fails a close or the session; a lookup (`Find()`) takes a lock
and does no I/O. `AssetProvider::get_end_ms` (called on the configuration work queue) makes the
`logs` provider add `end_ms` to the entries of `assets LIST` (see [smp.md](smp.md)), so a client shows
a log's duration without reading it.

### Status events and failures

The writer publishes `StatusUpdated` on the `logging_events` channel (see
[infrastructure.md](infrastructure.md)) with `IsActive` and, when inactive, `Reason`
(`LoggingStatusReason`):

| Reason | When |
| --- | --- |
| `Stopped` | `Stop()` finalized and closed the file. |
| `WriteFailed` | A record or a sync did not reach the card: the stream refuses every write after one that failed or fell short (a full or removed card). |
| `NoSpace` | Less than `EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` was free at a rotation. |
| `RotationFailed` | The next file of a rotation could not be opened or its header written. |

A fifth reason, `Unavailable`, comes only from other units' status (see
[Status on other units](#status-on-other-units)). `GetReasonName()` (`event_bus/logging_status_reason.h`)
names every reason for logs and the shell, `Unavailable` included, and gives `"unknown"` for a value
from a newer unit.

A failure ends the session on the writer thread: the error is counted in `write_errors` (not for
`NoSpace`) and logged once, the failure is published, and no further tick is scheduled.
`HasFailed()` reports it until the next `Start()`. The service stays `RUNNING` without a tick:
the writer never stops its own service, because `Stop()` waits for the running tick.
`LogWriterSupervisor` stops it on the configuration work queue when the event arrives (see
[Supervision](#supervision)); that stop finalizes what it can and closes the file (a file whose
stream failed stays readable as unfinalized), then publishes `Stopped`. Events are published with
`PublishAsync()`, so subscribers run on the bus thread, never on the writer thread or inside a
transition of the service. The channel drops every publish until a bus carries it: the composition
root registers it with one of its buses, and rt_core has no bus of its own for it. The active event
goes out from inside `Start()`, before the service reports `RUNNING`.

### Restarts

A new sensors or ADC configuration restarts sensor processing, which replaces the generation
the frame's readings belong to. After `LoggingController::Start()` such a store first stops
logging (see [Configuration updates](#configuration-updates)), and the restart below finds the
writer stopped. What follows is what a store does while the unit starts, and what it does to a
start requested after the update's stop. The supervisor's dependent (`GetDependentService()`), registered
with `SensorsController::RegisterDependentService()` (see [sensors.md](sensors.md)), stops the
writer before that generation goes and starts it again on the new one, while logging is still
wanted: the file is finalized and the next one describes the new sensors. A CAN reconfiguration
also restarts processing, without the writer; the configurations, and so the file layout, are the
same, and the drop count goes on across the frame's reset. `LoggingConfigurationManager` calls its
updated handler after `Update()` and `ApplyCborConfiguration()`, on the thread that stored the
configuration; the supervisor's handler restarts a running writer with the new interval, size
limit and sensor selection, in the same two cases. The handler can be replaced or cleared from any
thread while a store calls it.

### Supervision

`LogWriterSupervisor` (`services/log_writer_supervisor.h`) runs the writer of a unit that logs.
It implements `ILoggingControl`, the interface the inputs use (see [Control](#control)).

A request stores the wanted state in an atomic and reschedules the supervisor's one task on the
configuration work queue. It takes no lock, so it never waits for the writer, and a bus thread or
the CDMP work queue can make it. Two requests before the task runs act once, as the later one.
Each run of the task:

1. stops a session that failed: the writer's `HasFailed()` decides, not the event, so a failure
   event that arrives after a new start stops nothing;
2. applies the pending request;
3. restarts a running writer if a logging configuration was stored, unless step 2 opened a
   session, which already uses it.

The rules:

- A start while running does nothing. A stop while stopped does nothing and publishes nothing.
- A start of a failed session stops it and opens a new file. A start that cannot stop it starts
  nothing.
- A failed start publishes nothing and leaves logging stopped; nothing retries it.
- A stored logging configuration restarts a running writer and leaves a stopped or failed one
  alone, where no update stop came first: after `LoggingController::Start()` the update's stop is
  requested before the store's handler runs, so the writer is stopped for good instead (see
  [Configuration updates](#configuration-updates)). A start requested after that stop is the later
  request and wins, and the restart applies to it.
- A sensors restart starts the writer again only while logging is wanted: set by a successful
  start, cleared by a stop or a failure. A stop requested during the restart is still pending
  when the dependent is started, so the dependent does not start the writer.
- `HandleLoggingCommand(is_start)` is the `LOGGING` command from another unit: a start or a stop
  request, accepted either way.

| Code | Thread |
| --- | --- |
| `RequestStart(target_uid)`, `RequestStop(target_uid)`, `IsActive()` | Any; no lock. |
| Status handler | The bus thread that carries `logging_events`. Records the status; on a failure it reschedules the task. |
| Updated handler | The thread that stored the logging configuration (the configuration work queue for an SMP apply). Reschedules the task. |
| Every writer `Start()` and `Stop()`, the logger factory | The configuration work queue. |

Every start therefore runs on the configuration work queue: the free-space check, which may scan
the FAT of a large card, the logger's construction, the file open and the header write. Size that
queue's stack for it; rt_daq gives it 8 KiB. The dependent works only on the queue that
`SensorsController` restarts processing on, so both get the same one.

**Logger metadata.** The supervisor sets the writer's logger factory. At every start it builds
`Mdf4LoggerSensorReading` from the stored logging and sensors configurations, with:

| Field | Source |
| --- | --- |
| `device_name` | `CONFIG_BOARD` |
| `tool_id` | `LoggingController`'s `tool_id` (`"rt_daq"` on rt_daq), passed on to the supervisor's constructor. |
| `tool_version` | `EERIE_LEAP_SW_VERSION_MAJOR.MINOR.PATCH` |
| `product_id` | `EERIE_LEAP_PRODUCT_ID` |
| `hw_version`, `sw_version` | `SystemConfiguration`, packed. |
| `device_id` | The supervisor's `DeviceIdProvider` callback. `LoggingController` gives it one that reads the COM service's CDMP device ID; 0 without a COM service. |
| `sensors_configuration_hash` | The CRC of the stored sensors configuration, 0 when it was rejected at boot. |

The callback keeps the writer free of CDMP at compile time; `LoggingController` builds it from the
COM service. The factory, and the callback with it, lives as long as the writer, so the callback
holds the COM service weakly:

```cpp
[weak_com = std::weak_ptr<CanbusComService>(canbus_com_service)] {
    const auto com = weak_com.lock();
    const auto network_info = com != nullptr ? com->GetNetworkInfo() : nullptr;

    return network_info != nullptr ? network_info->GetDeviceInfo().device_id : uint8_t{0};
}
```

**Teardown.** The destructor withdraws the status subscription, the updated handler and the task.
A handler already dispatched to it does nothing (a `DispatchGuard`), and a run in progress is
waited for. It leaves the writer as it is, for its owner to stop (`LoggingController` does, see
[Wiring](#wiring)). The dependent and the factory hold the writer, the wanted state and the
managers, not the supervisor, so they stay harmless after it. No request may come during or after
the destructor: whoever requests keeps the supervisor alive or goes first.

## Control

`ILoggingControl` (`control/i_logging_control.h`) is the seam between every input and the writer:

| Method | Meaning |
| --- | --- |
| `RequestStart(target_uid)`, `RequestStop(target_uid)` | Record the wanted state and return, from any thread. The latest request wins. `target_uid` is the CDMP UID of the unit to start or stop; `ILoggingControl::ALL_UNITS` (0) is every unit that can log, this one included in the writer role. The supervisor ignores it: routing is `WriterLoggingControl`'s. A change of state arrives as `StatusUpdated` on `logging_events`; a request that changes nothing publishes nothing, such as a stop while stopped or a start that fails, so a requester cannot tell a refusal from a request not yet served. |
| `HandleLoggingCommand(is_start)` | The CDMP `LOGGING` command another unit sent (see [The `LOGGING` command](#the-logging-command)). Whether this unit accepts it: the answer is `SUCCESS` if so, else `FAILURE`. A unit that logs takes it as a request; a unit without a writer serves only a stop. |
| `IsActive()` | `IsActive` of the last `StatusUpdated` on `logging_events`; false until the first. For a shell's status and for tests: a trigger never reads it. |

A request never waits for the writer or for the CAN bus. Both implementations store the wanted
state in an atomic and reschedule one task on the configuration work queue, so a trigger can call
from a bus thread and the CDMP command from the CDMP work queue. The output needs no interface:
the status is `StatusUpdated` on `logging_events`, which a display or a widget binds to.

| Input | Source |
| --- | --- |
| [Triggers](#triggers) | Stored configuration: an event binding, typically an indicator sensor. |
| [The `LOGGING` command](#the-logging-command) | CDMP, from another unit; on every unit with a COM service. |
| [A configuration update](#configuration-updates) | A configuration stored on this unit: a stop. |
| The application's shell or autostart | `LoggingController::GetControl()`. |

The build picks the implementation (`EERIE_LEAP_DOMAIN_LOGGING`):

| Role | Control | Unit |
| --- | --- | --- |
| Writer | `WriterLoggingControl` (`control/writer_logging_control.h`): `LogWriterSupervisor` (see [Supervision](#supervision)) for this unit, and a `RemoteLoggingControl` in `Peer` mode for the others. Every unit with a writer can start and stop the other units too. | rt_daq |
| Remote | `RemoteLoggingControl`: a request becomes the CDMP `LOGGING` command. | rt_gauge |

`WriterLoggingControl` routes by target and holds no state or thread: `ALL_UNITS` is this unit's
writer and one broadcast (so a trigger on a unit that logs starts every other unit too), this unit's
own UID (`LoggingController::GetUnitUid()`) is the writer alone, and any other UID is a unicast
through the network half. Without a COM service there is no network half: only the writer is served,
and a request for another UID is logged and dropped. A received command goes to the writer, and a
stop also tells the network half to drop its wanted starts.

### Remote control

`RemoteLoggingControl` (`control/remote_logging_control.h`) is the control of a unit without a log
writer.

- **Targets.** A request names its target. `ALL_UNITS` is one broadcast, whatever the device table
  holds. A UID is one unicast to the device ID of the unit with that UID, looked up through
  `ICdmpNetworkInfo` when the command is sent: the unit must be online and `CanLog()` (its
  features carry `LOGGING`; `control/logging_unit.h` is the one place that says so). The control keeps one wanted
  state per target: slot 0 for all units and a slot per UID (at most one per trigger; a request that
  finds none is logged and dropped). A request for all units replaces every slot, since the broadcast
  reaches the others too. A start for a UID that is not online is held and sent when that unit
  appears, by the rules below; a stop for it is only logged, since an offline unit is not reachable
  and no signal would deliver the stop later. A stop for a UID that follows a start for all units
  is that unit's latest request: no re-assertion starts it again (a restart report skips it, and the
  status rule starts the other units one by one instead of broadcasting) until a later request moves
  it. A request for this unit's own UID is dropped with a
  warning: it has no writer.
- **Peer mode.** The network half of a writer is the same class with `Mode::Peer`: it subscribes to
  no status (the writer's own status is local, so `IsActive()` is false and the writer control
  answers it) and has no status rule. Its restart monitor is built with `has_status_rule = false`, so
  it reports every returning unit itself, also in the spell without any copy that the rule would
  have covered.
- **The send.** A request stores the wanted state and reschedules its task on the configuration
  work queue, which sends the `LOGGING` command through `CanbusComLoggingCommandSender`.
  `CanbusComService::SendCommand()` starts the transaction and sends the CAN frame on the calling
  thread, so a send never runs on the thread of a request. Two requests before the task runs send
  one command, the later one.
- **The result.** The response only logs a failure, also when no logging unit answers in time. A
  change of state is the shared status, which the importer publishes (see
  [Status on other units](#status-on-other-units)), and `IsActive()` follows it. A start that
  fails on the logging unit leaves the status as it was: the failure is logged on that unit only,
  and the response does not report it.
- **A wanted start is sent again.** A start sent while no unit logs is lost: the gauge's switch is
  turned on before the logging unit has booted, a trigger is already on at boot (a `Level` switch
  or the ignition), or the logging unit is away or rebooting, and a switch does not change again.
  So the control remembers that a start is wanted, from `RequestStart()` until `RequestStop()` or a
  `LOGGING` stop from another unit (below), and sends it again on two signals and on no other:
  - **The status.** It goes from inactive `Unavailable` to inactive `Stopped`: a logging unit
    appears. One broadcast when all units want a start, else a start to each target that resolves.
  - **The restart monitor** (below) reports a logging unit that restarted or joined and shares an
    inactive `Stopped` status again. One start to that unit, when all units want one or its own target
    does (a held start is delivered here); it used to be a broadcast.

  A stop from the logging unit's shell, or a failure such as `NoSpace`, stands until the switch
  moves, except where one of these follows it. Causes that meet in one run of the task send one
  start.
- **A stop clears the wanted start.** A configuration stored on this unit (`RequestStop()` from the
  update stop, see [Configuration updates](#configuration-updates)) and a `LOGGING` stop from another
  unit (`HandleLoggingCommand(false)`) drop it, so no rule below sends a start that undoes the
  stop. Only a new `RequestStart()` wants one again. The control sends nothing for a received stop:
  the other unit sent it to every unit. It drops the wanted start of every target, since it cannot
  tell a broadcast from a unicast. A start that is not sent yet is dropped; a stop of its own
  that is not sent yet stays due. A start from another unit is not served (`FAILURE`): this unit has
  no writer.

| Case | What sends it again |
| --- | --- |
| A logging unit was away longer than the heartbeat timeout (3 x 3 s) and returns, or joins | The status: `Unavailable`, then `Stopped`. When another unit reports, the status never passes through `Unavailable`, and the monitor sends it once the record is current. |
| A logging unit restarts and announces its record after its first heartbeat | The status: `Unavailable`, then `Stopped`. When another unit still reports, the monitor sends it once the record is back. |
| A logging unit restarts within the heartbeat timeout and already holds its record in its first heartbeat | The monitor, once the unit's record is current and `Stopped`: at once when it logged before, and at its next heartbeat (up to 3 s) when it was idle and its record is unchanged. A stop made on its shell before the restart is overridden. |
| A logging unit restarts while another unit logs | The monitor, as above. The combined status stays active, so no status event follows. |
| Several logging units restart or join together | The status, for the first unit to be back; the monitor, once for each of the others when its record is current. |
| A logging unit that had failed (`NoSpace`) restarts | The monitor: it comes back `Stopped`, and the wanted start is sent again. |
| A logging unit restarts and the monitor first finds its record active (it started by itself) or a failure | Nothing: the monitor consumes the restart. |
| A logging unit's own COM service restarts (a CAN configuration applied on it) | The monitor, at the unit's first heartbeat after its claim, since its records survive, unless the update's `LOGGING` stop cleared the wanted start first: it leaves before the unit claims its ID again. A stop made on its shell is overridden here too: see below. |
| This unit's own COM service restarts, as a CAN reconfiguration does, which drops every other unit's record (its own records survive) | The status: `Unavailable`, then `Stopped`, over a stop made on the logging unit's shell, unless the CAN configuration that caused the restart cleared the wanted start (it is an update). |
| A configuration stored on this unit, or a `LOGGING` stop from another unit | Nothing, until a new start is requested. |

A log writer shares `Stopped` when its exporter is built, before its own triggers, autostart or a
failed start can say more, so a restarted `rt_daq` gets at most one start, and normally one. That
start is redundant when its own trigger starts logging, and fails again when what failed before
fails still. A restart is consumed without a start only when the monitor first finds the unit's
record already active or a failure, which happens when the writer's own trigger starts it before
that.

**A COM restart is not told from a reboot.** A logging unit whose COM service restarts claims its ID
again with its UID, as a reboot does, and CDMP reports both as `RESTARTED`. The record versions
would tell them apart: a COM restart keeps the unit's versions, and a reboot usually starts them at
1 again. They do not always: a status record that was not stamped again since boot (the unit shared
`Stopped` at boot, and nothing changed since, for example after a start that failed, which publishes
nothing) has version 1 either way. So the monitor reports both, and a stop made on the logging
unit's shell before a CAN configuration was applied on it is overridden by the wanted start. The
CAN configuration is itself an update (L9): it stops logging on every unit and clears the wanted
start, so what a COM restart adds is only the restart without an update in between.

**The restart monitor.** `CanbusComLoggingRestartMonitor`
(`control/canbus_com_logging_restart_monitor.h`) tells the control that a logging unit is back. The
status cannot: a unit that restarts within the heartbeat timeout keeps its copies, stale, and the
importer publishes only a change of the combined status. A unit that was idle shows no change at
all, one that logged goes from active straight to `Stopped`, and with another unit reporting nothing
passes through `Unavailable`. So the monitor follows the unit on its own `LOGGING_STATUS` copy. The
control takes it as `ILoggingRestartMonitor`, so tests give it a fake. It registers a device event,
a heartbeat and a state handler with the COM service (`RegisterDeviceEventHandler()`,
`RegisterHeartbeatHandler()`, `RegisterStateHandler()`), all on the CDMP work queue.

- **Armed** when a unit joins (`CdmpDeviceEvent::JOINED`) or claims its ID again with its UID
  (`CdmpDeviceEvent::RESTARTED`), and its features carry LOGGING, which may share a status, or this unit holds
  a `LOGGING_STATUS` copy of it: the record is what makes a unit a logging unit, whatever its features.
  A unit that joined has no copy yet, so its features are all there is to go by. Two claims before the
  unit is current are one restart. A unit with LOGGING that never shares a status stays armed until it leaves
  the table, which costs a bit. With no copy of another unit, the status passes through
  `Unavailable` when the unit reports, which the status rule covers; with one, the monitor is the
  only signal.
- **Resolved** at the first check that finds the unit's copy current again
  (`RemoteStateRecord::is_stale` false). A state handler call finds it when the new record changed
  the copy's value, such as a unit that logged and is `Stopped` now: at once. A record equal to the
  stale copy renews it without a state handler call, so the unit's next heartbeat finds it, as it
  does when the copy was confirmed by the unit's digest: at most one heartbeat interval (3 s) after
  the record. Inactive `Stopped` is reported once. An active status or a failure consumes the
  restart without a report. What a unit shares before that check is seen only if it changes the
  value: a unit that started by itself with a record equal to the stale copy, and stopped again
  before its next heartbeat, is reported, and the wanted start overrides that stop.
- **Features.** The monitor goes by `ProductFeature::LOGGING` in a unit's features, which a unit
  with a log writer and a mounted log volume sends in its heartbeat. A unit that joins or restarts
  with LOGGING set is armed at the event. A DAQ sets the bit when its writer is built, after its
  first heartbeat, so the join may carry it clear: a heartbeat that shows LOGGING newly set (it was
  clear at the join, restart or last heartbeat) arms the unit too, whether its record came before or
  comes after that heartbeat. So does a status copy the unit held at the event.
- **Ready.** A current copy means the unit can take the `LOGGING` command: the writer role builds
  the command handler before the exporter that shares the record. A command that reaches a unit
  without the handler is dropped, so the monitor never reports on the claim alone.
- **Forgotten** when the unit leaves the table (`LEFT`). While no unit has a copy at all, the
  combined status is `Unavailable`, and the status rule sends the start when it returns to
  `Stopped`. The monitor keeps its units armed through that spell and sends nothing for the
  records that end it, since the rule's one broadcast reaches every unit that has a copy. The units
  that have no copy yet stay armed, and are reported when their own records are current: the rule
  sends once, and a unit that is still booting has no `LOGGING` handler yet. A heartbeat resolves
  nothing during the spell, since a record can be applied and its heartbeat handled before the state
  handlers run for it. When a unit that is back shares an active status or a failure, the combined
  status that ends the spell is not `Stopped`, the rule does not send, and the units that are back
  and `Stopped` are reported instead. This unit's own COM restart drops every copy and the device
  table without an event, so it ends in the same way: the units stay armed, and the records that
  return are covered by the rule.
- **A stable CDMP UID.** The monitor relies on it (`hwinfo`, the eFuse MAC on the ESP32 units). A
  unit without a hardware ID gets a random UID per boot, so its reboot is a new unit that claims
  another ID, not a `RESTARTED`, and the monitor never reports it.
- **A handler that throws** is caught and logged: a report runs inside the network service's
  heartbeat round, which would skip the handlers after the monitor's. The monitor's own handlers
  throw nothing: the network service's loops catch nothing, and the state service's catch and log.

### The `LOGGING` command

`LoggingCommandHandler` (`control/logging_command_handler.h`) serves the CDMP `LOGGING` command on
every unit with a COM service, in both roles. It passes the command to the control's
`HandleLoggingCommand()`, which decides:

| Role | A stop | A start |
| --- | --- | --- |
| Writer (a unit that logs) | `RequestStop()`, answered `SUCCESS` at once, which means accepted, not stopped; it also drops the wanted starts of the network half. | `RequestStart()`, answered `SUCCESS` at once: a change of state arrives as the shared status, and a start that fails leaves it as it was (logged on this unit only). |
| Remote (a unit without a writer) | Drops the wanted start and sends nothing; `SUCCESS`. | Not served: `FAILURE`, nothing changes. |

A unicast is answered by its unit alone, so a targeted request has no first-answer ambiguity.
A request without a payload is answered `FAILURE`, and so is one that arrives while the handler is
being destroyed. With two units in the remote role, a broadcast start can complete on the other
one's `FAILURE`: the sender then logs "Logging start command failed." although the unit that logs
started. The handler runs on the CDMP work queue and never waits for the writer, so a command cannot
hold up heartbeats or SMP over CAN (see [cdmp.md](cdmp.md#com-service)).

A unit does not hear its own broadcast on a real bus. The loopback controller of the tests hands a
unit its own frames, so its handler answers them there.

Order matters on a unit that logs: the supervisor's `Initialize()`, then the handler, then the
exporter. The exporter shares the first status as soon as it exists, and a command that arrives with
no handler is dropped without an answer. A request passed to a supervisor that is not initialized is
refused. The handler's destructor unsets whatever `LOGGING` handler the COM service holds, not only
its own, so destroy an old handler before building its replacement.

### Configuration updates

A configuration stored on a unit stops logging on every unit (L9). Whatever it is, ADC, CAN bus,
sensors, logging, the logging triggers, system, UI or display, a file never spans two
configurations of the network, and nothing starts logging again until a start is requested.

`ConfigurationUpdateLoggingStop` (`control/configuration_update_logging_stop.h`) is the rule. It
takes the stored handler of the unit's `ConfigurationService` (see
[configuration.md](configuration.md#configuration-service)), which every registered manager calls
after each successful store, and on the storing thread, before the store's own handler queues its
work, does the following in this order:

1. Makes the triggers take the next first value of each as a baseline. For a sensors update it
   also withdraws the triggers whose sensor is gone (see [Triggers](#triggers)). This is
   `LoggingController`'s update handler.
2. Calls `RequestStop(ALL_UNITS)` on the unit's `ILoggingControl`.
3. Where the control does not send the stop itself, queues the `LOGGING` stop broadcast on the
   configuration work queue. In both roles the control sends it: the writer's network half, the
   remote control. Several stores before it runs send one stop.

Because the stop is requested first, the supervisor's task runs before the restart the store
queues (the sensors restart, the restart for a logging configuration), and nothing starts the
writer again. The broadcast leaves before a CAN reconfiguration stops the COM service, which waits
`EERIE_LEAP_CONTROLLER_CANBUS_RECONFIGURE_DELAY_MS`.

| Store | Counts |
| --- | --- |
| An SMP `config` write of any registered type, over CAN, BLE or the serial console | Yes. |
| A manager's `Update()` or `Adopt()` after `Start()`, such as rt_daq's `logging config` and `logging reapply-sensors` shell commands | Yes. |
| The bitrate a CAN bus detected, written back to the CAN configuration | Yes. It is stored on the configuration work queue, not on the bus's thread. |
| The same bytes stored again | Yes. |
| A write that is rejected, malformed or fails to store | No. |
| A default created at first boot, and what the composition root stores before it calls `Start()` (the gauge's test configurations and its logging button) | No. A reboot of a unit does not stop the logging of another. |
| A setting the unit keeps for itself, stored with `UpdateOwnSetting()`: the gauge's brightness | No. |
| A configuration whose manager is not registered with the unit's `ConfigurationService` | Not reported. The gauge's system configuration is such a one: it is not writable over SMP and nothing stores it at run time. |

| | Writer role (rt_daq) | Remote role (rt_gauge) | No logging controller |
| --- | --- | --- | --- |
| The unit's own stop | `RequestStop(ALL_UNITS)` on the writer control: the supervisor stops the writer for good and clears the wanted state, and the network half sends the stop. | `RequestStop()` on the remote control: the wanted start is cleared at once, and its task sends the `LOGGING` stop. | None. |
| The broadcast | One broadcast, sent by the writer's network half (a `RemoteLoggingControl` in `Peer` mode). | Sent by the remote control. | The update stop's task: a unit without an SD card still tells the units that log, since its sensors feed their files. |
| Received `LOGGING` stop | `RequestStop()` on the supervisor, and the network half drops its wanted starts. | Drops the wanted starts. | No handler. |

**`Start()`.** The update stop registers nothing until `LoggingController::Start()`. The composition
root calls it once the unit has finished starting, after the last of its boot stores, before
`ManagementController` lets another unit write a configuration. What the unit stores before that
stops nothing. rt_daq's bench shell can store earlier, on its own thread: such a store stops
nothing either.

**Consequences.**

- A switch that is on stays on after an update and starts nothing (see [Triggers](#triggers)).
- With no other unit online the broadcast logs "Logging stop command failed." (the sender's
  warning).
- A unit that misses the frame, because it is offline or its own COM service restarts, keeps its
  wanted start and re-asserts it by the rules of [Remote control](#remote-control).
- The stop ends a session another trigger, the shell or another unit started.

### Triggers

`LoggingTriggerSet` (`control/logging_trigger_set.h`) subscribes the configured triggers. A trigger
is an event binding (see [infrastructure.md](infrastructure.md#event-bus-eerie_leap_event_bus)): a
channel, an event type, a payload key and an optional selector. The value it names, read as a bool,
is the wanted logging state.

- **One mode.** 0 to 1 requests a start and 1 to 0 a stop. A momentary button becomes a switch in
  its sensor (`Toggle`, see [sensors.md](sensors.md#indicators-as-logging-triggers)), not in the
  trigger, so a widget bound to the same sensor shows the same switch.
- **Values.** A `bool`, or an `int`, `uint32_t` or `float` that equals exactly 0 or 1. Anything else
  (NaN, another number, text) is ignored, the last state is kept, and it is logged once per
  trigger.
- **Changes only.** A trigger acts on changes of the value it saw last. At boot the first value it
  reads counts only when it is 1: a switch that is already on starts logging, and a switch that is
  off asserts nothing, so a unit whose switch is off does not stop a session that another trigger
  or another unit started. After a configuration update (see
  [Configuration updates](#configuration-updates)) the first value is a baseline, whatever it is:
  the update stopped logging, and nothing starts it until a later 0 to 1. A trigger that knows its
  value keeps it. Every trigger of the unit is a baseline from the first update until the unit
  restarts, also a sensor that publishes for the first time later.
- **Requests only.** A trigger only requests, and the latest request from any source wins. A
  switch left on does not prevent a stop from the shell or from another unit; its next change acts
  again. The one exception is the remote control's single send, above. The replay of a rebuilt set
  (below) starts logging only at boot: after an update it makes the baseline current.
- **Several triggers** coexist, such as a button and ignition. Each acts on its own changes. At most
  16.
- **A trigger that cannot act is logged when the set is built.** A channel this unit did not
  register, `None`, or a selector that cannot select subscribes nothing. On the `Sensors` channel
  with a `SensorId` selector the controller also checks the stored sensors: no enabled sensor with
  that id logs "selects nothing on this unit". That trigger stays subscribed, and acts as soon as a
  sensor with that id publishes.
- **A sensor that goes away is forgotten.** At every sensors update the controller checks the
  stored sensors again. A trigger whose sensor was removed, disabled or renamed is withdrawn: it
  logs one warning, forgets its last value and ignores values, the old generation's last ones
  included, until a later update finds the sensor again. Its first value then is a baseline too.
  The check runs at the store, before processing restarts. Only a sensors update re-checks the
  selectors: a CAN signal that goes away keeps its last value, and its return is itself a CAN
  update.
- **A stored configuration rebuilds the set.** The store of the triggers is an update: it stops
  logging, and the controller then replaces the set on the configuration work queue, with
  baselines, and asks the sensor events publisher to replay (see
  [sensors.md](sensors.md#sensor-events)), so the new set sees the current value of its switch as
  its baseline. A switch that is on starts nothing. Before the first update the replay still makes
  a switch that is on start logging, as at boot. Stores that come before the rebuild runs rebuild
  once. A rebuild that fails, such as one that runs out of memory, is logged and leaves no trigger
  acting until the triggers are stored again or the unit restarts.
- **Handlers hold a guard.** The bus calls handlers outside its subscriber lock, so a delivery
  already under way survives an unsubscribe. The handlers hold the set's `DispatchGuard`, and the
  destructor detaches it, waiting for a handler in its body, before it unsubscribes.

The gauge's button as configuration: a debounced switch sensor, and a trigger on its value.

```
sensors:          { id: "logging_button", type: PHYSICAL_INDICATOR, channel: 0,
                    debounce_ms: 30, input_mode: Toggle }
logging control:  { triggers: [ { channel: Sensors, event_type: DataUpdated, payload_key: Value,
                                  selector_key: SensorId, selector_value: "logging_button" } ] }
```

### Threading

Logging control adds no thread: its code runs on the threads below. In the writer role the
controller builds the log writer, and the writer's own work queue (priority 8,
`EERIE_LEAP_LOG_WRITER_STACK_SIZE`, see [How it works](#how-it-works)) is the only thread the
controller brings. The remote role brings none.

| Code | Thread |
| --- | --- |
| The controller's constructor, `Initialize()`, getters and destructor | The composition root's. |
| The update stop's report (the triggers' baselines, `RequestStop()`) | The thread that stored the configuration: the configuration work queue for an SMP apply and the shell's commands, a test's thread otherwise. The CAN bus's detected bitrate is handed to the configuration work queue first. |
| The updated handler of the triggers' configuration | The thread that stored it (the configuration work queue for an SMP apply). It only reschedules the rebuild. |
| The rebuild of the triggers and the replay | The configuration work queue. |
| Trigger handlers | The bus thread of the bound channel (the sensors' bus for a sensor), or the caller of `Publish()`. |
| The supervisor's task, `RemoteLoggingControl`'s send (both modes), the update stop's broadcast, the availability reporter's poll | The configuration work queue. Requests, from any thread, take only `RemoteLoggingControl`'s spinlock around its per-target state; the task copies it and sends outside. |
| Status handlers (supervisor, remote control, exporter) | The bus thread that carries `logging_events`. |
| `LoggingCommandHandler` (both roles), the importer, `CanbusComLoggingRestartMonitor`'s handlers and its report into `RemoteLoggingControl` | The CDMP work queue. |

### Stored triggers

The triggers are `ConfigurationService::Type::LoggingControl`, type 8. It is separate from the
logging configuration, which only a unit with a log writer has. Schema
(`logging_control_config.cddl`), file `config/logging_control_config.cbor`:

```
CborLoggingControlBindingConfig = [ channel: uint, event_type: uint, payload_key: uint, has_selector: bool, selector_key: uint, selector_value: CborSelectorInteger / tstr / bool, target_uid: uint ]
CborSelectorInteger = -2147483648..4294967295
CborLoggingControlConfig = [ triggers: [0*16 CborLoggingControlBindingConfig] ]
```

| Field | Rules and meaning |
| --- | --- |
| `channel` | An `EventChannelId` (1 sensors, 2 logging, ...). A channel the unit does not register makes the trigger do nothing; a client's apply refuses it. |
| `event_type`, `payload_key` | The channel's enum values: for `Sensors`, `DataUpdated` (0) and `Value` (1). A client's apply refuses a value past the channel's last. |
| `has_selector` | Whether the selector applies. Without it `selector_key` and `selector_value` are ignored; `selector_value` is written as 0. |
| `selector_key`, `selector_value` | The payload key the selector matches and its value: for a sensor, `SensorId` (0) and the sensor's text id (matched by its hash), an integer or a bool. An integer is -2^31..2^32-1 (a hash above 2^31-1 may be written as is; it is kept as the same 32 bits, and a larger integer does not decode). A trigger on `Sensors` must have a `SensorId` selector. |
| `target_uid` | The CDMP UID (`CdmpDeviceInfo::uid`) of the unit the trigger starts and stops; 0 is every unit that can log. |

Every field is required, so a file stored before `target_uid` existed does not decode (the file is kept and an unsaved default, no triggers, is used until a valid one is stored), and a stored list of more than 16 triggers does not decode. Any `target_uid` is valid: the validator has no rule for it. The validator
checks the count, that each channel is an `EventChannelId`, that a selector is text, an integer
or a bool, and that a `Sensors` trigger selects by `SensorId`. That is structural and runs on load,
save and apply: it does not resolve channels or sensors. When a client applies a configuration, the
manager also asks the unit (`GetEventBindingError()`, passed by the controller as its
`EventBindingChecker`) and refuses the first trigger whose channel this unit does not register, or
whose event type, payload key or selector key its channel does not have: "Invalid logging control
configuration. Trigger 2 names an event type its channel does not have." Loading and `Update()` skip
that check, so a stored configuration still loads. The default is no triggers.

## Status on other units

A unit that logs shares its status with every other unit on the CDMP network, and a unit that
shows the status, such as the gauge, receives it as ordinary `StatusUpdated` events on its own
`logging_events` channel. Its consumers, a widget binding or a controller, cannot tell them from
local events. The status travels as a shared state record (see [cdmp.md](cdmp.md#shared-state)),
moved by the state bridge with the descriptor `LoggingNetworkState`
(`event_bus/logging_network_state.h`):

- **On the unit that logs**, `NetworkStateExporter<LoggingNetworkState>` shares the record from its
  construction, inactive and `Stopped` until the first event, then follows the writer's events.
  Its destruction withdraws the record.
- **On a unit that shows the status**, `NetworkStateImporter<LoggingNetworkState>` combines every
  other unit's record whenever one changes, and once right after its construction, and publishes
  the result on `logging_events` when it differs from what it published last.

```cpp
// LoggingController, writer role (rt_daq), once a bus carries logging_events: shares the writer's status.
logging_status_exporter_ = std::make_unique<NetworkStateExporter<LoggingNetworkState>>(canbus_com_service_);

// LoggingController, remote role (rt_gauge), once a bus carries logging_events: shows the status of the units that log.
logging_status_importer_ = std::make_unique<NetworkStateImporter<LoggingNetworkState>>(canbus_com_service_);
```

Both take the COM service as their `INetworkStateTransport` and throw `std::runtime_error` when
they cannot subscribe to the channel or register with the transport. A unit either exports or
imports the status, never both.

A bus has to carry `logging_events` first, because the channel drops every event until one does.
On an inert channel the exporter keeps sharing its initial record, and the importer publishes
nothing. The importer does not count a state it could not publish, so the next change of a record
after a bus registers the channel publishes the combined status, even if that status did not
change. If the transport refuses the exporter's record because every record slot holds another
state type, the exporter offers it again with each later event.

**The record** is `LOGGING_STATUS` (0x10), `CanbusComLoggingState` in the COM domain:

| Byte | Content |
| --- | --- |
| 0 | Flags. Bit 0: active. Bits 1-7: 0, ignored by the decoder. |
| 1 | The `LoggingStatusReason` while inactive, 0 while active. A record of one byte is inactive for the reason `Stopped`. |

Bytes after byte 1 are ignored, so fields can be appended.

**A unit's record** follows its `StatusUpdated` events, with one exception: an inactive `Stopped`
never replaces an inactive failure. The supervisor's stop that cleans up after a failure publishes
`Stopped` milliseconds later (see [Status events and failures](#status-events-and-failures)), and
the record keeps the failure until the next start. A stop of a writer that is already stopped
publishes nothing, so that cleanup is the only `Stopped` that can follow a failure.

**The combined status**, which the importer publishes, is:

- active while any unit logs;
- otherwise inactive, with the reason of the lowest device ID that reports a failure
  (`WriteFailed`, `NoSpace` or `RotationFailed`), else `Stopped`;
- inactive with the reason `Unavailable` when no unit reports a status: no logger is online, or
  this unit's CDMP node is not, since a unit that leaves `ONLINE` drops every other unit's record.

`Unavailable` (4) is only ever imported; the writer never publishes it. Code that switches over
`LoggingStatusReason` handles it too.

**Timing.** A change reaches the other units with one notification, or with a resend within
350 ms when that is lost, and otherwise with the next heartbeat (3 s) and a replay. A logger that
disappears (cable, power) leaves the combined status after the heartbeat timeout (9 s) plus up to
one validation interval (5 s). A logger that reboots keeps its last status, stale, until it leaves
the device table or sends its new record; `RemoteStateRecord::is_stale` marks such a copy, and the
[restart monitor](#remote-control) uses it.

**Threading.** No thread is added:

- The exporter handles events on the thread of the bus that carries `logging_events` (the
  application's bus). Sharing hands the sending over to the CDMP work queue.
- The importer runs on the CDMP work queue, never blocks, and publishes with `PublishAsync()`, so
  consumers run on the importing unit's bus thread (the gauge's `AppEventBus`), as for local events.
  It keeps its buffers of every unit's record as members, off the work queue's stack.
- Imported events carry the bridge's `IMPORTED_EVENT_SOURCE_ID`, which exporters skip, so a unit
  never shares back what it imported.

The descriptor, the record and the bridge are header-only: a unit that only shows the status, such
as the gauge, builds them without `EERIE_LEAP_DOMAIN_LOGGING`, with `EERIE_LEAP_DOMAIN_CANBUS_COM`,
`EERIE_LEAP_EVENT_BUS` and a bus that carries `logging_events`.

## Loggers

`ILogger<SensorReading>` has `StartLogging(stream, start_time)`, `LogReading(time, reading)`
(`time` is the caller's current time, see above), `StopLogging()`, `GetBytesWritten()` and the
file extension. `LogReading()` throws only
`std::ios_base::failure`, when the stream refuses a record; `StopLogging()` never throws and
returns false when the file could not be finalized. One implementation exists:

**`Mdf4LoggerSensorReading`** (used by rt_daq). Built from a `LoggingConfiguration`, the sensors
and a `LoggerMetadata` (device name, tool ID and version, product ID, CDMP device ID, packed
hardware and software versions, sensors configuration hash), all copied at construction: a
reading is matched by its sensor ID hash only and its sensor pointer is never followed. It
describes the sensors as they were when it was built, which is why the service builds one
through its factory at every start. The file is written with the "unfinalized" identifier and
patched on `StopLogging()`, so a file cut by a power loss stays readable. `StartLogging()` seals
the layout into one record writer per group; each record (a CAN frame together with its payload
record) then reaches the stream's buffer in a single write, without a heap allocation, and the
header is serialized along the block chains in loops, so its stack depth does not grow with the
sensor count.

- **Time.** The master channel `Timestamp` of every group is Float64 seconds since the start
  time in the header, the record time minus the start time (0 for a reading older than the
  file). Float64 resolves microseconds over any session length; readings are stamped at the
  kernel tick's resolution (100 µs on the ESP32-S3 boards, see
  [infrastructure.md](infrastructure.md)), so frames that arrive within one tick share a time.
  Within a value group the times strictly increase: a sample stamped before the tick that last
  wrote its predecessor again (it was still being processed) is moved to 1 µs after that
  record.
- **Record IDs.** One byte while every group fits (2 x enabled sensors + 1 ≤ 255; a `CANBUS_RAW`
  sensor uses two groups), else two bytes.
- **Names.** Per enabled sensor one channel group named after the sensor ID. A value sensor's
  group holds the value channel, named after the sensor ID (Float32, unit from the metadata), and
  with `log_raw_value` on a `PHYSICAL_ANALOG` or `PHYSICAL_INDICATOR` sensor with an expression,
  the raw channel `<id>.raw`. A `CANBUS_RAW` sensor's group, also named after the sensor ID, is a
  `CAN_DataFrame` bus-event group (`BusChannel` = the source's CAN interface index + 1, `ID`,
  `IDE`, `DLC`, `Dir`, `EDL`, `BRS`, `ESI`, `DataLength`, `DataBytes` as a 64-bit VLSD offset)
  backed by a VLSD group for the payloads; asammdf's bus extraction reads it. Each frame's
  payload record is written before its frame record, so a file cut between the two ends in a
  payload no frame points at, which readers skip, rather than in a frame pointing past the end.
- **Comments.** The value channel's comment (the CAN group's, for a raw sensor) is the sensor name,
  followed by `: ` and the description when there is one. The header comment is
  `Eerie Leap sensor log` with the common properties `device_name`, `product_id`, `device_id`,
  `hw_version`, `sw_version`, `sensors_configuration_hash` and `logging_interval_ms`, in decimal;
  the file history carries the tool ID and version with the vendor `Eerie Leap`.
- **Conversion.** The raw channel gets an algebraic conversion only when a reader evaluates it
  the way the expression engine does: the only identifier is `x`, and the expression uses
  nothing but `+ - * /`, parentheses, decimal numbers (no leading zero, which numexpr rejects)
  and `sin cos tan sqrt abs log`. The formula is the expression with `x` renamed to `X`
  token-wise and `a ^ n` written as the product `(a * ... * a)` when `a` is a single token and `n`
  a whole number from 1 to 4 (asammdf's numexpr has no `pow()`; other powers are not
  converted): `x * 4 + 1.6` becomes `X * 4 + 1.6`, `x ^ 2 * 0.5` becomes `(X * X) * 0.5`. Any
  other expression goes into the raw channel's comment as `expression: <text>`.
- **Invalidation bits.** The value channel and the raw channel each have an invalidation bit
  (bits 0 and 1 of one byte after the record's data, so with a one-byte record ID value records
  are 14 bytes, 18 with the raw value; the master channel and CAN groups have none). A reading
  with an error (`HasError()`) or without a value is written, not skipped: the value field is 0
  and the value channel's bit is set, so the stale value an error reading may still carry never
  looks valid. A missing raw value is written as 0 with only the raw channel's bit set; an error
  reading's raw value, when it has one, stays valid (the input was measured, a later step
  failed). The service logs each sensor's latest processed reading, or its latest reading when
  that is an error (`SensorReadingsFrame::SnapshotLoggableReadings()`), so a sensor whose readings
  fail shows invalid samples from the first failed one until it recovers, not its last good value
  held.
- **Invalid samples in asammdf** (8.8). `MDF.get(name)` honours the bits by dropping the invalid
  samples, timestamps included (`invalidation_bits` is `None`); `get(name,
  ignore_invalidation_bits=True)` returns every sample, the 0 placeholders included, with the
  bits in `Signal.invalidation_bits` (`True` = invalid; `validate()` drops those samples).
  `to_dataframe()` interpolates across the dropped samples unless `use_interpolation=False`,
  which leaves NaN there, and `MDF(path, ignore_invalidation_bits=True)` reads the placeholders
  as values. The GUI opens files with the bits honoured unless its setting says otherwise.
- **Readings.** The tick offers every sensor's latest sample each time. A sample whose
  timestamp was already written is skipped with `log_only_new_data`, and otherwise written
  again at the tick time (sample and hold at the logging interval); a reading without a
  timestamp is always written. An invalid reading follows the same rules: its own timestamp
  makes it a new or a held sample, and a held one is written invalid again. Raw CAN frames ignore
  the flag: every queued frame is written.

## Stored configuration

Schema (`logging_config.cddl`), file `config/logging_config.cbor`, type 5 in the `config`
group:

```
CborSensorLoggingConfig = [ sensor_id_hash: uint, is_enabled: bool, log_raw_value: bool, log_only_new_data: bool ]
CborLoggingConfig = [ logging_interval_ms: uint, max_log_size_mb: uint, sensor_configs: [*CborSensorLoggingConfig] ]
```

| Field | Default | Rules and meaning |
| --- | --- | --- |
| `logging_interval_ms` | 100 | ≥ 10. Period of the writer's tick. |
| `max_log_size_mb` | 10 | ≤ 1024. A file that reaches this size is finalized and the next one opened (see [Rotation and free space](#rotation-and-free-space)); 0 never rotates. |
| `sensor_configs[].sensor_id_hash` | | 32-bit hash of the sensor ID (see [smp.md](smp.md)). A sensor without an entry is not logged. |
| `is_enabled` | false | Log this sensor. A sensor disabled in the sensors configuration (see [sensors.md](sensors.md#sensor-model)) is not logged either way. |
| `log_raw_value` | false | Also log the value before the expression (MDF4: the `<id>.raw` channel). |
| `log_only_new_data` | false | Skip a tick when the reading's timestamp has not changed since the last record of this sensor; off, the held sample is written again at the tick time. A reading without a timestamp is always logged. Does not apply to `CANBUS_RAW` sensors, whose every frame is logged. |

The default configuration logs nothing.

## Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_DOMAIN_LOGGING` | n | Builds the domain. Depends on `EERIE_LEAP_CONFIGURATION` and `EERIE_LEAP_DOMAIN_SENSOR` (the readings frame, the sensors configuration the loggers describe); selects `EERIE_LEAP_EVENT_BUS`, `EERIE_LEAP_MDF` and `EERIE_LEAP_DOMAIN_SYSTEM` (the product ID and versions in the log header). |
| `EERIE_LEAP_LOG_WRITER_STACK_SIZE` | 8192 | Stack of the writer's work queue. A rotation finalizes a file and writes the next header on it (3648 bytes on the ESP32-S3), and a failed write unwinds an exception on it (rt_daq uses 6144). |
| `EERIE_LEAP_LOG_SYNC_INTERVAL_MS` | 0..3600000, 1000 | How often a tick flushes and syncs the log file; bounds what a power loss can take. 0 syncs only when logging stops. |
| `EERIE_LEAP_LOG_MIN_FREE_SPACE_MB` | 0..1048576, 16 | Free space needed to start logging and to rotate to the next file. 0 disables the check. |
| `EERIE_LEAP_DOMAIN_LOGGING_STATS` | n | Logs the writer's counters once per period: ticks, readings, frames, drops, bytes, stream writes, syncs, rotations, overruns, errors, tick duration. The counters are always kept (`GetStats()`). |
| `EERIE_LEAP_LOG_STATS_LOG_PERIOD_S` | 1..3600, 10 | Period of the statistics line, and the shortest interval between two dropped-frame warnings. |
| `EERIE_LEAP_LOG_DATA_FILE_PREFIX` | `"log_data"` | File name prefix; empty drops the prefix and the underscore. With `EERIE_LEAP_ASSETS` the prefix plus the 26 characters the writer adds must be a valid asset name (see [Log files as assets](#log-files-as-assets)); the build checks it. |
| `EERIE_LEAP_MDF` | n | Builds the MDF 4 writer; selected by `EERIE_LEAP_DOMAIN_LOGGING`, and usable without it. |
| `EERIE_LEAP_CONTROLLER_LOGGING` | n | Builds `LoggingController`, logging control and the triggers' configuration, with or without the logging domain, which picks the role. Depends on `EERIE_LEAP_CONFIGURATION`, `EERIE_LEAP_EVENT_BUS`, `EERIE_LEAP_DOMAIN_SENSOR` (the publisher, the trigger values) and `EERIE_LEAP_DOMAIN_CANBUS_COM` (the command handler, the importer, the remote role); selects `EERIE_LEAP_THREADING`. A configuration stored after `LoggingController::Start()` stops logging on every unit. |

## Wiring

`LoggingController` (`controllers/logging_controller.h`, `EERIE_LEAP_CONTROLLER_LOGGING`) builds
logging control for the role the build picks. The composition root builds it once, after
`CanbusController` and `SensorsController::Initialize()`, before `ManagementController` and before
`SensorsController::Start()`, so the triggers subscribe first. It calls `Start()` once the unit's
last boot store is done, before `ManagementController` (see `Start()` below).

**Before the controller.** The bus and the registry entries come before anything that publishes or
subscribes. The publisher, and `CanbusController` with `SensorsController::Initialize()`, come
before the controller, in any order:

1. A bus for each of `logging_events` and `sensor_events`, registered by the composition root
   before anything publishes on them (one bus may carry both, as rt_daq's `AppEventBus` does; the
   gauge has `SensorEventBus` for `sensor_events` and its own `AppEventBus` for `logging_events`):
   a channel drops every publish until then. The root also registers both channels with
   `EventChannelRegistry`, as `EventChannelId::Sensors` and `EventChannelId::Logging`; without the
   entries every trigger logs that its channel is not registered and does nothing.
2. The `SensorEventsPublisher` on the shared readings frame, built and started (see
   [sensors.md](sensors.md#sensor-events)). It is a constructor parameter, and `Initialize()` replays
   through it, so it can be built at any point before the controller.
3. `CanbusController` and `SensorsController::Initialize()`, which builds the sensors manager the
   controller reads.

**Constructor.**

```cpp
auto logging_controller = std::make_shared<LoggingController>(
    fs_service, config_work_queue_thread, configuration_service,
    sensors_controller->GetConfigurationManager(), canbus_com_service, sensor_events_publisher,
    // The writer role only:
    sd_fs_service, time_service, sensor_readings_frame, "rt_daq",
    // With EERIE_LEAP_ASSETS: the registry the log files are registered with.
    assets_manager);

// The writer role: the composition root registers the dependent, before SensorsController::Start().
if(logging_controller->Initialize() == 0)
    sensors_controller->RegisterDependentService(logging_controller->GetDependentService());

// Once the last of the unit's boot stores is done, before ManagementController lets another unit
// write a configuration: from here on a stored configuration stops logging on every unit.
logging_controller->Start();
```

| Parameter | Writer role | Remote role |
| --- | --- | --- |
| `fs_service`, `config_work_queue_thread` | Required. The queue is the one `SensorsController` has, running, so a sensors restart and the writer's transitions run in turn. | Same. |
| `configuration_service` | Null: the managers are not registered. One that already holds type 5 or 8 fails `Initialize()`: build the controller once per service. | One that already holds type 8 fails `Initialize()`. |
| `sensors_configuration_manager` | Required: the logger's sensors and metadata, and the check of a trigger's sensor. | Null: a trigger's sensor is not checked. |
| `canbus_com_service` | Null: the unit logs on its own, without the `LOGGING` command or a shared status. | Required: `Initialize()` fails without it. |
| `sensor_events_publisher` | Null: a rebuilt trigger waits for the next change of its value instead of a replay. | Same. |
| `sd_fs_service`, `time_service`, `sensor_readings_frame` | Required. | Ignored. |
| `tool_id` | The log header's tool ID. | Ignored. |
| `assets_manager` | With `EERIE_LEAP_ASSETS` and the SD service: the registry the `logs` kind is registered with once `Initialize()` succeeded. Null: not registered. | Ignored: the remote role writes no logs. |

**`Initialize()`** returns 0, or -1 with every getter null and nothing registered. It runs once. It
builds, in this order:

| Step | Writer role | Remote role |
| --- | --- | --- |
| 1 | The triggers' configuration manager (`config/logging_control_config.cbor`). | Same. |
| 2 | The test hook, `Initialize(setup)`, may store triggers: before the set exists, so it rebuilds nothing. | Same. |
| 3 | The logging configuration manager (`config/logging_config.cbor`), `LogWriterService` (initialized), `LogWriterSupervisor` (initialized), then, with a COM service, the network half (`CanbusComLoggingRestartMonitor` with `has_status_rule = false`, then `RemoteLoggingControl` in `Peer` mode, initialized), `WriterLoggingControl` over both, `LoggingCommandHandler` and the exporter. Without a COM service `WriterLoggingControl` has the supervisor only. A failed exporter is logged and not fatal. | `CanbusComLoggingRestartMonitor`, then `RemoteLoggingControl` (initialized, with `GetNetworkInfo()`), then `LoggingCommandHandler`, then the importer. The monitor, the handler and the importer fail `Initialize()` when the COM service refuses a handler. |
| 4 | The rebuild task. | Same. |
| 5 | The trigger set, then a replay if any trigger subscribed. | Same. |
| 6 | The triggers' updated handler. | Same. |
| 7 | With a configuration service, `ConfigurationUpdateLoggingStop` (initialized; it registers nothing before `Start()`), without a sender in both roles: the control sends the stop. |
| 8 | Registration with `ConfigurationService`: logging (type 5), then logging control (type 8). | Logging control (type 8). |
| 9 | With `EERIE_LEAP_ASSETS`, a registry and the log volume: the `logs` kind (see [Log files as assets](#log-files-as-assets)). | Nothing. |

Registration is the last step that can fail, because `ConfigurationService` cannot unregister, so a
failed `Initialize()` leaves nothing behind. The `logs` kind follows it: a registration the registry
refuses, or that throws, is only logged and does not fail `Initialize()`; `Teardown()` takes the kind
back. `ConfigurationService` also keeps the first manager of a type, and a controller
built on a service that holds one would work against a manager nothing reacts to: `Initialize()`
refuses it before it builds anything. The replay at step 5 finds an empty frame when sensors are not started yet;
it makes a controller built after `SensorsController::Start()` still see a switch that is already on.

**`Start()`** returns 0, or -1 after a failed `Initialize()` and before `Initialize()`. It registers
the update stop with the configuration service, and from then on a stored configuration stops
logging on every unit (see [Configuration updates](#configuration-updates)). It runs once: a second
call logs an error and returns the first result. Without a configuration service it returns 0 and
no store stops anything. A unit without a logging controller (rt_daq without an SD card, or after a
failed `Initialize()`) builds a `ConfigurationUpdateLoggingStop` with a sender and no control,
initializes it and calls its `Start()` at the same place, so what it stores still stops the units
that log.

**Availability.** In the writer role with a COM service, `CanbusComLoggingAvailabilityReporter`
keeps the LOGGING feature that the unit sends on CDMP equal to `IsAvailable()` of the log volume:
it sets the bit in its constructor, so the first heartbeat carries it, polls every second on the
configuration work queue (not the CDMP one: `IsAvailable()` takes the filesystem mutex, which the
writer holds across SD syncs) and clears the bit when torn down. A write failure or a full volume
does not clear it; the logging status reports those. A unit without a writer never sets it.

**Getters.** `GetControl()` is the role's `ILoggingControl`;
`GetLoggingControlConfigurationManager()` serves both roles. The writer role adds
`GetDependentService()`, `GetConfigurationManager()` (the logging manager) and
`GetLogWriterService()`. After `Initialize()` returned 0, the composition root registers
`GetDependentService()` with `SensorsController::RegisterDependentService()`, before
`SensorsController::Start()`; the controller does not, as it never sees `SensorsController`. A
failed controller registers nothing and the root skips what depends on it.

**Teardown.** The destructor withdraws everything that refers to the controller, in an order that is
safe while the bus threads and the configuration and CDMP work queues run. It also runs after a
failed or skipped `Initialize()`. The `logs` kind is unregistered first, so no request asks the
writer about a file past this.

1. Destroys the update stop: its handler is cleared, a report in its body is waited for, and a
   broadcast still pending is dropped.
2. Detaches the updated handler's guard and clears the handler, then cancels a pending rebuild and
   waits for one in progress.
3. Destroys the trigger set and the command handler: nothing requests from here.
4. Writer role: requests a stop through the supervisor and waits until the configuration work queue
   has run it, so the wanted state is cleared and the dependent that `SensorsController` keeps
   cannot start the writer again. A writer is never dropped while it runs. Then the exporter
   withdraws the record, so the other units see `Unavailable`; they may never see the `Stopped`,
   which is published asynchronously. The availability reporter goes with the exporter and clears LOGGING.
5. Destroys the control and what it drives. Remote role: the importer first, then the control,
   which waits for a send in progress, clears the restart monitor's handler and drops the monitor,
   which unregisters from the COM service and waits for a handler in progress.

Destroy it on the thread that built it. It waits for the configuration work queue, the bus threads
and the CDMP work queue, so it must not run on any of them. On the configuration work queue it logs
an error and stops the writer directly instead of waiting for itself. Whoever kept `GetControl()`
releases it before the controller goes, so no request comes in after the teardown.

**Other inputs.** The shell and an autostart call `ILoggingControl`, and a display subscribes to
`logging_events`.

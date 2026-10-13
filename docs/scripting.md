# Expressions and Lua scripts

Two ways to compute with sensor values at run time: a compiled arithmetic expression per sensor,
and Lua scripts attached to sensors and CAN messages.

## Expressions (`EERIE_LEAP_EXPRESSION_ENGINE`)

Code: `src/subsys/expression_engine/`, engine in `modules/expression_engine/`. The language
reference is [`modules/expression_engine/docs/LANGUAGE.md`](../modules/expression_engine/docs/LANGUAGE.md).

A sensor's `expression` is compiled once, when the configuration is validated and again when the
pipeline is built, into a few dozen bytes of bytecode. Evaluation per sample allocates nothing,
throws nothing and takes no lock.

- Values are `float`. Arithmetic, comparisons (`1`/`0`), `&&`, `||`, `?:`, `^` (power),
  bitwise `& | ~ << >>` and `xor()`, hex and binary literals, and the usual math functions
  (`sin`, `sqrt`, `abs`, `min`, `max`, `avg`, `rint`, ...).
- `x` is the sensor's own input: the interpolated value of a `PHYSICAL_ANALOG`, the level (0/1)
  of an indicator, the decoded signal of a CAN sensor, the script's value of a `USER_*` sensor.
  Virtual sensors have no `x`.
- Every other identifier is a sensor ID. The validator rejects an unknown one; the pipeline
  builder binds each to the address of that sensor's latest value in the readings frame, so the
  expression reads it directly.
- An expression is evaluated only when every sensor it reads has a value; otherwise the reading
  gets `INPUT_UNAVAILABLE`. A NaN result gives `EXPRESSION_NOT_A_NUMBER`.

Examples: `x * 4 + 1.6`, `sensor_1 < 400`, `(raw >> 8) & 0xFF`, `temp > 90 ? 1 : 0`.

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_EXPRESSION_ENGINE` | n | Builds the evaluator; selects `EXPRESSION_ENGINE`. Needs `CONFIG_STD_CPP23`. |
| `EXPRESSION_ENGINE_MAX_LENGTH` | 256 | Longest expression text. |
| `EXPRESSION_ENGINE_MAX_INSTRUCTIONS` | 64 | Bytecode instructions per expression. |
| `EXPRESSION_ENGINE_MAX_VARIABLES` | 16 | Distinct identifiers per expression, `x` included. |
| `EXPRESSION_ENGINE_MAX_STACK` | 16 | Evaluation stack slots. |
| `EXPRESSION_ENGINE_MAX_NESTING` | 16 | Parser recursion limit. rt_core compiles with a limit of 8 because configurations are parsed on the configuration work queue, whose stack is small. |
| `EXPRESSION_ENGINE_MAX_ARGUMENTS` | 8 | Arguments of `sum`, `avg`, `min`, `max`. |

## Lua scripts (`EERIE_LEAP_LUA_SCRIPT`)

Code: `src/subsys/lua_script/` (the host), `src/domain/script_domain/` (the sensor globals),
interpreter in `modules/zephyr_lua/`.

Scripts are plain Lua files on the SD card, referenced by a path relative to the card's mount
point (`scripts/sensor_8.lua`) in a sensor's or a CAN message's `script_path`. A client lists
`scripts/` with the SMP `files` group to pick one (see [smp.md](smp.md)). On a unit with an SD
service the validators reject a configuration whose script file is not on the card, so while no
card is mounted every non-empty `script_path` is rejected; a unit without an SD service checks no
`script_path` and runs no script.

### Hosting

- **One state per sensor generation.** All scripted sensors of a configuration generation share
  one Lua state. Each script is loaded into its own environment table that falls back to the
  globals, so it sees the libraries and the functions below but not another script's globals, and
  its own globals keep their values between calls. The entry points are looked up once, when the
  pipeline is built; a script that fails to compile or run is logged and the sensor runs without
  it.
- **One state per CAN message.** A message with a script gets its own state, loaded when the CAN
  configuration is parsed.
- **Memory.** A state is allocated on the external heap and every allocation is counted against
  `EERIE_LEAP_LUA_SCRIPT_MEMORY_LIMIT_KB`. Past the limit the allocation fails inside the Lua call
  that needed it, which reports a memory error; the other scripts are not affected.
- **Threads.** Sensor functions run on the sensor processing thread, CAN functions on the CAN
  scheduler thread. A script must return quickly and must not block. Callers that share a state
  take its lock around every use.
- **Libraries.** The host opens the base library (`pairs`, `ipairs`, `tostring`, `tonumber`,
  `pcall`, `error`, ...; present when `LUA_LIB_BASE` is set, the default) and `table`, `string`,
  `math` and `utf8`. `print` is replaced by the host's version and `sleep_ms` is added.
  `coroutine`, `package`, `io`, `os` and `debug` are not opened. The base library's `loadfile`
  and `dofile` go through the libc's `fopen`, which only works on targets with POSIX file I/O
  (`POSIX_DEVICE_IO`, `POSIX_FD_MGMT`); elsewhere they fail with "Operation not supported",
  so scripts are loaded through `script_path`, not from inside another script.

### Functions a script can call

Available to sensor and CAN message scripts.

| Function | Returns | Meaning |
| --- | --- | --- |
| `get_sensor_value(sensor_id)` | `number` or `nil` | The latest processed value of the sensor with that ID; `nil` when it has no value yet or does not exist. |
| `update_sensor_value(sensor_id, value)` | `boolean` | Overwrites the latest value of another sensor; `false` when the sensor does not exist. To change the sensor the script belongs to, return the new value from `post_process_sensor_value` instead. |
| `print(message)` | | Writes one string to the console. |
| `sleep_ms(ms)` | | Sleeps. Avoid it in sensor and CAN scripts, where it stalls the calling thread. |

### Functions a sensor script can define

| Function | Called | Meaning |
| --- | --- | --- |
| `create_sensor_value(sensor_id) -> number` | At the sampling rate of a `USER_ANALOG` or `USER_INDICATOR` sensor | Produces the reading, in place of an ADC or GPIO read. Without it the sensor keeps producing readings without a value. |
| `post_process_sensor_value(sensor_id, value) -> number \| nil` | Once per reading of any sensor type, after interpolation and after the expression | Receives the reading's current value (`nil` when it has none). A number replaces the value before it is committed; `nil` keeps it. A Lua error marks the reading `SCRIPT_FAILED`: it is committed with the error and its dependents keep their previous values. |

A `USER_ANALOG` sensor that counts up:

```lua
-- State variables persist across calls.
local counter = 0

function create_sensor_value(sensor_id)
    counter = counter + 1
    return counter
end
```

A `PHYSICAL_ANALOG` sensor whose value is clamped and offset after interpolation, and a sensor
that reads another sensor:

```lua
function post_process_sensor_value(sensor_id, value)
    if value == nil then
        return nil
    end

    local rpm = get_sensor_value("engine_rpm")
    if rpm == nil then
        return -1          -- the other sensor has no value yet
    end

    return math.min(value, 100.0) * rpm
end
```

### Functions a CAN message script can define

`process_can_frame(frame_id, data) -> string`: called before every send of the message, after
its signals have been encoded. `data` holds the frame bytes as a string of exactly
`message_size` characters, one byte per character; a message without signals arrives as zeros.
The function must return a string of the same size, which is sent instead; an empty string sends
the original bytes.

A frame with a gear value and a checksum, cycling through the gears every 1000 sends:

```lua
-- State variables persist across calls.
local counter = 0
local alive_counter = 0
local current_gear = 0

local GEARS = {
    [0] = 0,   -- clear screen
    [1] = 1, [2] = 2, [3] = 3, [4] = 4,
    [5] = 5,   -- auto / D
    [6] = 6,   -- neutral
    [7] = 7,   -- reverse
    [8] = 9,   -- 5th
    [9] = 10   -- 6th
}

local function checksum(gear_info, alive_cnt)
    local tmp = ~(alive_cnt ~ gear_info)      -- NOT (alive XOR gear)
    return (((tmp & 0x0f) << 4) | alive_cnt) & 0xFF
end

function process_can_frame(frame_id, data)
    counter = counter + 1
    if counter >= 1000 then
        counter = 0
        current_gear = (current_gear + 1) % 10
    end

    local gear_value = GEARS[current_gear]
    local bytes = {0, 0, 0, 0, 0, 0, 0, 0}

    bytes[2] = gear_value & 0x0F                     -- byte 1, bits 0-3: gear
    bytes[4] = checksum(gear_value, alive_counter)   -- byte 3: checksum with alive counter
    alive_counter = (alive_counter + 1) % 16

    return string.char(table.unpack(bytes))
end
```

### Kconfig

| Symbol | Default | Meaning |
| --- | --- | --- |
| `EERIE_LEAP_LUA_SCRIPT` | n | Builds the host; selects `LUA`. |
| `EERIE_LEAP_LUA_SCRIPT_MEMORY_LIMIT_KB` | 256 | Memory budget of one state; 0 is unlimited. |
| `EERIE_LEAP_DOMAIN_SCRIPT` | n | The `get_sensor_value` and `update_sensor_value` bindings; selected by the sensor domain. |
| `LUA_32BITS` | y | `float` numbers and 32-bit integers. |
| `LUA_LIB_BASE` | y | Compiles the base library; the host opens it when present. `table`, `string`, `math` and `utf8` are always built and opened. |
| `LUA_LIB_COROUTINE`, `LUA_LIB_PACKAGE`, `LUA_LIB_IO`, `LUA_LIB_OS`, `LUA_LIB_DEBUG` | n | Compiled into the interpreter on request, but the host does not open them. |
| `EERIE_LEAP_LUA_LOG_LEVEL_*` | | Log level of the Lua module. |

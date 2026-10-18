# EerieLeap Common

A repository containing common code used in EerieLeap projects.

## Documentation

How each subsystem and domain works and how to configure it: [`docs/README.md`](docs/README.md).

## Development

This repository is a Zephyr module and doubles as a west manifest repository, so
it can be opened and tested on its own.

```sh
git submodule update --init --recursive
```

Then reopen the folder in the dev container (`.devcontainer/`). The image runs `west init -l` / `west
update` for you, so the workspace is ready once the container is up.

Copy `.vscode_example/` to `.vscode/` to pick up the editor tasks.

### Formatting

Sources follow [`.clang-format`](.clang-format), written for clang-format 23.1.1
(`pip install clang-format==23.1.1`, already in the dev container). `scripts/format.sh` checks
the tree, `scripts/format.sh --fix` reformats it and `--changed` limits either to changed files.
[`.clang-format-ignore`](.clang-format-ignore) lists generated and vendored code. The nested
modules carry the same config, and CI fails if the copies drift.

## Tests

```sh
west twister -c --disable-warnings-as-errors -j 4 -O ./twister-out -T ./tests
```

`-j` caps parallel builds and QEMU instances; without it twister uses every core
and can starve the container. Narrow a run with `-t <tag>` (`unit`, `functional`,
`utilities`, `subsys.adc`, ...) or `-T <suite path>`.

### Platforms

| Platform          | Architecture | Bits | Endianness |
| ----------------- | ------------ | ---- | ---------- |
| `native_sim`      | x86          | 32   | little     |
| `qemu_cortex_a9`  | ARMv7-A      | 32   | little     |
| `qemu_cortex_a53` | AArch64      | 64   | little     |
| `qemu_riscv64`    | RISC-V       | 64   | little     |
| `qemu_malta`      | MIPS         | 32   | little     |

Each suite lists the platforms it runs on in `platform_allow`. Suites needing a
flash or ADC backend pick up `qemu_flash.*` / `qemu_adc.*` from `tests/` by
setting `RT_CORE_TEST_SIM_FLASH` or `RT_CORE_TEST_ADC_EMUL` before including
`rt_core_test.cmake`. The QEMU ADC overlay includes a third ADC whose channel
states no resolution, which rt_core's ADC `Initialize()` rejects; a scenario
that needs every ADC accepted sets `RT_CORE_TEST_NO_FAILING_ADC` in its
`extra_args` (`functional.adc_domain.smp` does). Every QEMU build also gets
`qemu_stack_checks.conf` (`CONFIG_STACK_SENTINEL`): work queue stacks come from
the heap, unguarded on all of them but `qemu_riscv64`, so an overflow names the
thread instead of surfacing later as heap corruption.

Every suite builds and passes on every board it allows. A Kconfig option that
compiles code calling into an option it does not enable may still link on some
boards, where the linker drops the unused code, and fail on others; the full
matrix is what catches it.

Some cases skip by design. `DumpFileForAsammdf` is a manual dump (set
`CONFIG_MDF_TEST_DUMP_DIR`). `functional.subsys.fs.unbuffered` skips the
put-area cases, which run in `functional.subsys.fs`.

`functional.sensor_domain` runs on every board. Off `native_sim`, its
throwing-filter case needs a 12288-byte sensor events publisher stack: at the
4096 default it skips (it overflowed on `qemu_cortex_a53` while unwinding, and
8192 would leave about 236 bytes on `qemu_riscv64`). The
`functional.sensor_domain.raised_publisher_stack` scenario runs the suite on
every QEMU board at 12288, and the case checks how much of that stack the
publisher thread leaves unused after the unwind.

`functional.logging_control` runs on every board. Off `native_sim`, the restart
monitor's throwing-handler case (`test_AHandlerThatThrowsDoesNotSkipTheHandlersAfterIt`)
needs an 8192-byte CDMP work queue stack: at the 4096 default it skips (it
overflowed on `qemu_cortex_a53`, `qemu_riscv64` and `qemu_malta` while
unwinding). The `functional.logging_control.raised_cdmp_stack` scenario runs the
suite at 8192 on every QEMU board.

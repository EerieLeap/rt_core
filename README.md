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
| `native_sim`      | x86-64       | 64   | little     |
| `qemu_cortex_a9`  | ARMv7-A      | 32   | little     |
| `qemu_cortex_a53` | AArch64      | 64   | little     |
| `qemu_riscv64`    | RISC-V       | 64   | little     |
| `qemu_malta`      | MIPS         | 32   | big        |

Each suite lists the platforms it runs on in `platform_allow`. Suites needing a
flash or ADC backend pick up `qemu_flash.*` / `qemu_adc.*` from `tests/` by
setting `RT_CORE_TEST_SIM_FLASH` or `RT_CORE_TEST_ADC_EMUL` before including
`rt_core_test.cmake`. Every QEMU build also gets `qemu_stack_checks.conf`
(`CONFIG_STACK_SENTINEL`): work queue stacks come from the heap, unguarded on
all of them but `qemu_riscv64`, so an overflow names the thread instead of
surfacing later as heap corruption.

Known gaps: three suites do not link on every QEMU board. `unit.domain` and
`functional.system_domain` fail to link on `qemu_cortex_a53`, `qemu_riscv64`
and `qemu_malta` (`unit.domain` is missing `CONFIG_CRC`;
`functional.system_domain` leaves `CanbusService::GetMessageConfiguration`
undefined), and `unit.subsys.mdf` fails to link on `qemu_riscv64`
(`threading::Thread::Join()` undefined). The other suites build and pass on
every board they allow.

`functional.sensor_domain` runs on every board. Off `native_sim`, its
throwing-filter case needs an 8192-byte sensor events publisher stack: at the
4096 default it skips (it overflowed on `qemu_cortex_a53` while unwinding). The
`functional.sensor_domain.raised_publisher_stack` scenario runs the suite on
`qemu_cortex_a53` at 8192.

# EerieLeap Common

A repository containing common code used in EerieLeap projects.

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
`rt_core_test.cmake`.

Known gaps: `qemu_malta` cannot fit the heavier suites in RAM, `qemu_cortex_a53`
faults in the sensor and ADC emulation paths, and `unit.subsys.mdf` and
`functional.sensor_domain` are `native_sim` only.

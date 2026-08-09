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

The twister suites under `tests/` run on `native_sim`:

```sh
west twister -c --disable-warnings-as-errors -O ./twister-out -T ./tests
```

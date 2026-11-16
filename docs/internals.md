# Internals

## Source layout

| Path | Contents |
| --- | --- |
| `pyproject.toml` | Python package metadata, dependencies, and CLI entry point |
| `artemis_emu/` | CLI, GUI, test API, and USB device tools; `assets.json` pins downloads |
| `qemu/overlay/` | Watch and ESP32-S3 device models added to QEMU's source tree |
| `qemu/patches/` | ESP32-S3 integration, CPU pacing, and corrections to existing device models |
| `qemu/tools/` | Build scripts, register table generator, and patch editing tools |
| `flake.nix` | Nix packages, test runner, and development shell |
| `nix/qemu.nix` | Espressif QEMU build with the overlay and patches |
| `nix/micropython.nix` | CircuitMess MicroPython build with pinned ESP-IDF and compiler |
| `examples/` | MicroPython programs |
| `tests/` | Firmware, example, API, CLI, and GUI tests; `test_like_a_watch.py` also runs on hardware |

QEMU and the Python front end communicate over a Unix socket. The protocol is documented at the top of [`board.c`](../qemu/overlay/hw/artemis/board.c). Serial ports and QMP use separate Unix sockets in a temporary directory for each run.

The GUI uses Qt through PySide6, with native desktop styling and the same `Watch` API as the tests.

## Timing

CPU pacing counts instructions. Each core executes a few microseconds of guest code, then waits for the clock to catch up. Guest time excludes host work such as code translation and device model execution. Register accesses add delays based on hardware measurements.

Idle cores process timers as they become due, reducing interrupt delays caused by host thread scheduling. `--turbo` disables CPU pacing.

## Development

In a virtual environment, run `pip install -e '.[test]'` for an editable installation with pytest. Run the tests with `python -m pytest tests`.

After changing `qemu/`, rebuild with `qemu/tools/build.sh`. The emulator automatically uses the local build. See [build dependencies](install.md#building-qemu). Alternatively, `nix develop` provides dependencies and source builds, and `nix run .#test` runs tests against the Nix packages.

To edit the QEMU patch:

1. Run `qemu/tools/patch-trees.sh unpack DIR` to create the original tree at `DIR/a` and patched tree at `DIR/b`.
2. Edit files under `DIR/b`.
3. Run `qemu/tools/patch-trees.sh pack DIR` to regenerate the patch.

New device models go in `qemu/overlay/`. Changes to QEMU's existing files go in the patch.

Generate `qemu/overlay/hw/artemis/soc_registers.h` from ESP-IDF headers with `python3 qemu/tools/gen-soc-registers.py /path/to/esp-idf > qemu/overlay/hw/artemis/soc_registers.h`.

Nix flakes in a Git checkout exclude untracked files. Stage new files with `git add` before building.

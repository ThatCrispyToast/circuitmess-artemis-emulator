# How it's put together

```
pyproject.toml          the Python package: command line, window, real-watch support, and the API
artemis_emu/            its code; assets.json pins what it downloads
qemu/overlay/           the watch's devices and the chip's missing ones, added to QEMU's source tree
qemu/patches/           wires them into the ESP32-S3 machine, paces the CPUs, and fixes the model where it differs from the chip
qemu/tools/             builds that QEMU, generates the table of register reset values from ESP-IDF's headers, and unpacks the patch for editing
flake.nix               the same as Nix packages, built from source, and a development shell
nix/qemu.nix            Espressif's QEMU, plus the two directories above
nix/micropython.nix     CircuitMess's MicroPython, with its pinned toolchain and ESP-IDF
examples/               MicroPython programs
tests/                  runs the stock firmware, MicroPython, the examples and the command line; test_like_a_watch.py runs on a watch too
```

QEMU and the front end talk over a small protocol on a Unix socket, documented at the top of
`qemu/overlay/hw/artemis/board.c`. The serial ports and QMP are Unix sockets too, in a temporary
directory per run.

## Timing

The CPUs are paced by counting instructions: each core runs a few microseconds' worth, then
waits for the clock. The clock stands still while QEMU translates code or runs a device model,
since neither takes the chip any time, and a register access is charged what it costs on the
chip. Cores that are waiting run timers the moment they fall due, so interrupts aren't late by
however long a sleeping thread takes to wake. `--turbo` turns all of that off.

The window is Qt, through PySide6, in your desktop's own style, on top of the same `Watch`
object the tests use.

## Working on it

`pip install -e '.[test]'` in a virtual environment installs the emulator from the checkout
with pytest, and `python -m pytest tests` runs its tests. After a change under `qemu/`,
`qemu/tools/build.sh` builds a QEMU the emulator then uses instead of the published one;
[install.md](install.md) has what that needs. `nix develop` gives a shell with all of it built
from source, and `nix run .#test` runs the tests against that.

The patch to QEMU's own files is a diff, and is edited as the two trees it is the difference
of: `qemu/tools/patch-trees.sh unpack DIR`, change files under `DIR/b`, then `pack DIR`. New
devices go in `qemu/overlay/` and need no patch.

`qemu/overlay/hw/artemis/soc_registers.h` is generated:
`python3 qemu/tools/gen-soc-registers.py /path/to/esp-idf > qemu/overlay/hw/artemis/soc_registers.h`.

Nix flakes only see files tracked by Git, so `git add` new files before building with it.

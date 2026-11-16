# Third-party components

The GPL-2.0-or-later license in this repository covers its original code, examples, documentation, and build scripts. Upstream software and firmware retain their own licenses.

## QEMU

The emulator uses [Espressif QEMU](https://github.com/espressif/qemu), release `esp-develop-9.2.2-20260417`, with this project's patches and device models. QEMU as a whole is GPL-2.0; individual files may use other compatible licenses. The ESP32-S3 ROM image is included from the same upstream release.

The binary archive includes QEMU's `COPYING`, `COPYING.LIB`, and `LICENSE`, plus the copyright notices and license texts for each bundled system library, under `share/doc/qemu-artemis/`. Its `libraries.json` records the exact binary and source package versions.

Each release includes a matching QEMU source archive and an archive of the bundled libraries' source packages. These are available beside the binaries on the [release page](https://github.com/ThatCrispyToast/circuitmess-artemis-emulator/releases). The source archives include build instructions and preserve upstream notices.

## MicroPython

The firmware is built from [CircuitMess MicroPython](https://github.com/CircuitMess/micropython) commit `02bf6480cafe80b1c2ccfe420079bd83d9415c87`, using ESP-IDF 5.0.2 and the dependencies pinned in [`nix/micropython.nix`](nix/micropython.nix).

MicroPython is primarily MIT-licensed. ESP-IDF and other components use additional licenses. The release includes the pinned source trees and their license files, plus the toolchain's GCC and Newlib notices. A consolidated notices file is downloaded beside the firmware image.

## Stock firmware

CircuitMess's stock firmware is downloaded directly from [CircuitMess's releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases). It is not included in this repository or this project's release assets.

## Python dependencies

PySide6, pyserial, and esptool are installed separately by pip or Nix. Their distributions provide their own license files; they are not embedded in this project's Python wheel.

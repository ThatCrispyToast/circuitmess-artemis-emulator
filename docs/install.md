# Installation

The emulator uses a Python package, a patched QEMU build, and stock or MicroPython firmware. Prebuilt QEMU binaries support x86-64 Linux and Windows through WSL 2.

## Python package

Install Python 3.10 or later, `git`, and Qt's graphics and audio dependencies:

| Distribution | Packages |
| --- | --- |
| Ubuntu, Debian | `sudo apt install -y git python3-venv libegl1 libgl1 libglib2.0-0 libfontconfig1 libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-shape0 libwayland-cursor0 libpulse0 fonts-dejavu-core` |
| Fedora | `sudo dnf install -y git python3 glib2 mesa-libEGL mesa-libGL libxkbcommon-x11 xcb-util-cursor xcb-util-wm xcb-util-keysyms xcb-util-image xcb-util-renderutil libwayland-cursor pulseaudio-libs dejavu-sans-fonts` |
| Arch | `sudo pacman -S --needed git python glib2 libglvnd libxkbcommon-x11 xcb-util-cursor xcb-util-wm xcb-util-keysyms xcb-util-image xcb-util-renderutil wayland libpulse fontconfig ttf-dejavu` |

Desktop installations may already include these libraries. The emulator reports missing libraries at startup. `--headless` does not require the graphics or audio libraries.

From a clone of this repository:

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install .
artemis-emu stock
```

The Python environment includes PySide6 (Qt), pyserial, and esptool. It uses about 450 MB, or 700 MB on Python 3.10 and 3.11, which require an older PySide6 release.

### Windows

Run `wsl --install` in an administrator PowerShell session, then restart Windows. Open Ubuntu from the Start menu and follow the Ubuntu instructions above, including the graphics and audio packages. The emulator window and audio use the Windows desktop.

## Downloads

Files are downloaded on first use:

| File | From | Size |
| --- | --- | --- |
| QEMU for x86-64 Linux, glibc 2.34 or later | This project's releases | 5 MB |
| This project's build of CircuitMess's MicroPython | This project's releases | 2 MB |
| CircuitMess's stock firmware, per version | [CircuitMess's releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) | 4 MB |

Downloads are cached in `$XDG_DATA_HOME/artemis-emu`, defaulting to `~/.local/share/artemis-emu`. Set `ARTEMIS_HOME` to override this directory. Downloads are verified against the SHA-256 hashes in [`artemis_emu/assets.json`](../artemis_emu/assets.json).

`ARTEMIS_ASSETS_URL` sets a mirror URL for QEMU and MicroPython downloads. To use local files, set `ARTEMIS_QEMU`, `ARTEMIS_MICROPYTHON_FIRMWARE`, or `ARTEMIS_STOCK_FIRMWARE`.

## Building QEMU

Build QEMU for other architectures, older systems, or changes under `qemu/`:

```sh
qemu/tools/build.sh
```

The script downloads the pinned Espressif QEMU release, applies this project's device models and patch, and installs it in `~/.local/share/artemis-emu/qemu`. The emulator uses this build in preference to the prebuilt binary. Build dependencies:

| Distribution | Packages |
| --- | --- |
| Ubuntu, Debian | `sudo apt install -y build-essential ninja-build pkg-config python3-venv curl patch libglib2.0-dev libgcrypt20-dev libpixman-1-dev libslirp-dev zlib1g-dev` (and `python3-tomli` where Python is 3.10) |
| Fedora | `sudo dnf install -y gcc make ninja-build pkgconf-pkg-config python3 curl patch glib2-devel libgcrypt-devel pixman-devel libslirp-devel zlib-ng-compat-devel` |
| Arch | `sudo pacman -S --needed base-devel ninja python curl patch glib2 libgcrypt pixman libslirp zlib` |

`qemu/tools/build.sh --bundle FILE` packages QEMU with its libraries, notices, and corresponding sources. It requires Ubuntu or Debian with `patchelf`, `dpkg-dev`, and `deb-src` repositories enabled. See the [release instructions](releasing.md) for the Ubuntu 22.04 build environment.

Building MicroPython requires Nix: `nix build .#micropython-firmware`. The build pins ESP-IDF and the compiler.

## Nix

From a clone, `nix run . -- stock` builds pinned versions of QEMU and MicroPython, fetches the stock firmware, and starts the emulator. `nix develop` provides a development shell with the same components. Both supply the firmware and QEMU paths, so the emulator needs no runtime downloads.

## Uninstalling

Delete the clone, including `.venv`, and the data directory (`~/.local/share/artemis-emu` by default).

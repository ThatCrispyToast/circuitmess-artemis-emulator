# Installing

The emulator is a Python package, a build of QEMU of its own, and two firmware images. It runs on x86-64 Linux, and on Windows in WSL 2.

## The usual way

Python 3.10 or later, `git`, and the system libraries Qt draws windows and plays sound with:

| Distribution | Packages |
| --- | --- |
| Ubuntu, Debian | `sudo apt install -y git python3-venv libegl1 libgl1 libfontconfig1 libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-shape0 libwayland-cursor0 libpulse0 fonts-dejavu-core` |
| Fedora | `sudo dnf install -y git python3 mesa-libEGL mesa-libGL libxkbcommon-x11 xcb-util-cursor xcb-util-wm xcb-util-keysyms xcb-util-image xcb-util-renderutil libwayland-cursor pulseaudio-libs dejavu-sans-fonts` |
| Arch | `sudo pacman -S --needed git python libglvnd libxkbcommon-x11 xcb-util-cursor xcb-util-wm xcb-util-keysyms xcb-util-image xcb-util-renderutil wayland libpulse fontconfig ttf-dejavu` |

A desktop has most of them. Without one of them the emulator says which library is missing rather than opening a window, and `--headless` needs none of them.

Then, in a clone of this repository:

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install .
artemis-emu stock
```

`pip` brings PySide6 (Qt), pyserial and esptool into `.venv`, about 450 MB; on Python 3.10 and 3.11 it is an older PySide6 that takes 700 MB. Nothing is installed outside that directory.

### Windows

In PowerShell as administrator, `wsl --install`, and restart. Open Ubuntu from the Start menu, and install as for Ubuntu above: a new WSL has none of the desktop libraries, so it needs the whole list. The watch's window opens on the Windows desktop, with sound.

## What is downloaded

The first time each is wanted, the emulator fetches:

| File | From | Size |
| --- | --- | --- |
| Its QEMU, for x86-64 Linux with glibc 2.34 or later | this project's releases | 5 MB |
| CircuitMess's MicroPython, as built by this project | this project's releases | 2 MB |
| CircuitMess's stock firmware, per version used | [CircuitMess's releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) | 4 MB |

They are kept in `~/.local/share/artemis-emu` (`$XDG_DATA_HOME/artemis-emu`), or the directory `ARTEMIS_HOME` names. Each is checked against a SHA-256 in `artemis_emu/assets.json` before it is used. `ARTEMIS_ASSETS_URL` names another place to fetch this project's two from.

To do without them: `ARTEMIS_QEMU`, `ARTEMIS_MICROPYTHON_FIRMWARE` and `ARTEMIS_STOCK_FIRMWARE` name files to use instead.

## Building QEMU yourself

For another kind of computer, an older system, or after changing anything under `qemu/`:

```sh
qemu/tools/build.sh
```

It downloads Espressif's QEMU at the pinned release, adds this project's devices and patch, and installs the result in `~/.local/share/artemis-emu/qemu`, where the emulator prefers it to the published build. It takes a minute or a few, and needs:

| Distribution | Packages |
| --- | --- |
| Ubuntu, Debian | `sudo apt install -y build-essential ninja-build pkg-config python3-venv curl patch libglib2.0-dev libgcrypt20-dev libpixman-1-dev libslirp-dev zlib1g-dev` (and `python3-tomli` where Python is 3.10) |
| Fedora | `sudo dnf install -y gcc make ninja-build pkgconf-pkg-config python3 curl patch glib2-devel libgcrypt-devel pixman-devel libslirp-devel zlib-ng-compat-devel` |
| Arch | `sudo pacman -S --needed base-devel ninja python curl patch glib2 libgcrypt pixman libslirp zlib` |

`qemu/tools/build.sh --bundle FILE` packs it with its libraries, which is how the published build is made, on Ubuntu 22.04.

The MicroPython image is built by Nix alone, `nix build .#micropython-firmware`, since it takes a particular ESP-IDF and compiler.

## With Nix

`nix run . -- stock` in a clone builds QEMU and MicroPython from source at pinned versions, fetches the stock firmware, and runs the emulator with them; `nix develop` gives a shell with the same. Nothing is downloaded at run time.

## Removing it

Delete the clone, which holds `.venv`, and `~/.local/share/artemis-emu`.

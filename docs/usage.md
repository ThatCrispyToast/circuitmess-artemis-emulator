# Using the emulator

## Commands

```sh
artemis-emu stock                      # CircuitMess's factory firmware
artemis-emu python examples/hello.py   # a MicroPython program
artemis-emu repl                       # a MicroPython prompt (Ctrl-] quits)
artemis-emu run firmware.bin           # a firmware you built
```

`run` accepts a merged flash image, a bare application image (placed behind the stock firmware's bootloader and partition table), or an ESP-IDF project or build directory. `stock` runs CircuitMess's latest release; `--version 2.0`, `2.1`, `2.1.1` or `2.2` picks another.

Options, for any of these commands:

| Option | Effect |
| --- | --- |
| `--headless` | no window; a `python` run ends when the script does |
| `--timeout SECONDS` | stop after this long |
| `--screenshot FILE.png` | save the display on exit |
| `--flash FILE` | keep flash (settings, files, NVS) in a file between runs |
| `--time 2026-07-20T20:17:00` | set the clock; the default is your local time, and `unset` is a clock that was never set, as a new watch has |
| `--battery MV`, `--usb`, `--charging` | start with this power supply |
| `--revision 2` | emulate hardware revision 2 instead of 3 |
| `--turbo` | run code as fast as your computer can, instead of at the chip's speed |
| `--sensor-noise` | give the motion sensor the noise a real one has |
| `--mute` | don't play the buzzer |
| `--no-watchdogs` | keep watchdogs from firing, for debugging a hang |
| `--gdb PORT`, `--wait-gdb` | open a GDB stub, optionally waiting for it before starting |

`python` also takes `--install`, which stores the script as `main.py` so that it runs at every boot, the way a program from CircuitBlocks does:

```sh
artemis-emu python my_program.py --install --flash my_watch.bin
artemis-emu repl --flash my_watch.bin   # boots into main.py; Ctrl-C for a prompt
```

## In the window

| Input | Effect |
| --- | --- |
| Left, Right | lever |
| Enter or Space | push the lever in |
| Esc or Backspace | side button |
| W A S D, `0` | tilt the watch, level it |
| `T` | tap |
| `R` | reset (cuts power) |
| `M` | mute |
| F12 | screenshot |

The side panel has the same controls for the mouse: the lever and button, sliders for pitch and roll, tap, double tap and steps, and the battery voltage, USB cable and charging.

## Hardware revisions

Watches come in two revisions, with different displays. Revision 2 shipped with firmware 2.0; revision 3 needs 2.1 or later, and refuses 2.0. The emulator defaults to 3. `device info` says which a real watch is.

## With a real watch

Plug the watch in over USB and the same commands work on it:

```sh
artemis-emu device info                        # chip, revision, and what firmware it's running
artemis-emu device backup my-watch.bin         # save its whole flash, and do this first
artemis-emu device flash micropython           # put this project's MicroPython on it
artemis-emu python examples/hello.py --device  # run a program on the watch
artemis-emu repl --device
artemis-emu device monitor --reset             # watch it boot
artemis-emu device flash stock --version 2.0   # a CircuitMess release
artemis-emu device restore my-watch.bin        # exactly as it was
```

`device flash` also takes a firmware file. Flashing MicroPython keeps a MicroPython filesystem that is already on the watch, and clears whatever another firmware left where it goes; `--erase-files` starts clean either way. With several watches plugged in, `--port` picks one.

Your user needs access to the watch's serial port, which is an Espressif USB Serial/JTAG device (`303a:1001`). On most distributions that means joining the group that owns serial ports, and logging in again:

```sh
sudo usermod -aG dialout $USER   # the group is uucp on Arch
```

On NixOS, a udev rule does it:

```nix
services.udev.packages = [
  (pkgs.writeTextDir "lib/udev/rules.d/70-esp-usb-jtag.rules" ''
    SUBSYSTEMS=="usb", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"
  '')
];
```

Under WSL, the watch has to be handed from Windows to Linux first, with [usbipd-win](https://github.com/dorssel/usbipd-win). That route hasn't been tried with this project.

## Firmware

The MicroPython firmware is built from [CircuitMess's source](https://github.com/CircuitMess/micropython) for the board they maintain for their ESP32-S3 devices, unchanged. It keeps files in a FAT partition in the upper half of flash, so a program installed as `main.py` survives a power cycle, here and on a watch. Its CPU runs at ESP-IDF's default 160 MHz.

The stock firmware is downloaded from [CircuitMess's releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) when needed, not kept here.

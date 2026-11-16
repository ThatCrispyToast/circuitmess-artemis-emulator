# Using the emulator

## Commands

```sh
artemis-emu stock                     # CircuitMess's factory firmware
artemis-emu python examples/hello.py  # run a MicroPython program
artemis-emu repl                      # MicroPython prompt (Ctrl-] quits)
artemis-emu run firmware.bin          # custom firmware
```

`run` accepts a merged flash image, a bare application image, or an ESP-IDF project or build directory. Bare applications use the stock firmware's bootloader and partition table. `stock` defaults to the newest supported CircuitMess release; select a version with `--version 2.0`, `2.1`, `2.1.1`, or `2.2`.

Common options:

| Option | Effect |
| --- | --- |
| `--headless` | Run without a window; `python` exits when the script finishes, unless `--install` is set |
| `--timeout SECONDS` | Stop after the specified duration |
| `--screenshot FILE.png` | Save the display on exit |
| `--flash FILE` | Persist flash contents, including settings, files, and NVS, between runs |
| `--time 2026-07-20T20:17:00` | Set the RTC; defaults to local time. Use `unset` for an uninitialized clock |
| `--battery MV`, `--usb`, `--charging` | Set initial battery voltage, USB connection, and charging state |
| `--revision 2` | Emulate hardware revision 2; the default is 3 |
| `--turbo` | Disable CPU pacing and run at the host's maximum speed |
| `--sensor-noise` | Add noise to motion sensor readings |
| `--mute` | Mute the buzzer |
| `--no-watchdogs` | Disable watchdogs when debugging hangs |
| `--gdb PORT`, `--wait-gdb` | Start a GDB stub; optionally wait for the debugger to resume execution |

Use `python --install` to save a script as `main.py` and run it at boot. `--flash` preserves the file between emulator runs:

```sh
artemis-emu python my_program.py --install --flash my_watch.bin
artemis-emu repl --flash my_watch.bin  # boots into main.py; Ctrl-C for a prompt
```

## Window controls

| Input | Effect |
| --- | --- |
| Left, Right | Move the lever |
| Enter or Space | Press the lever |
| Esc or Backspace | Press the side button |
| W A S D | Tilt the watch |
| `0` | Reset tilt to level |
| `T` | Tap |
| `R` | Reset by cycling power |
| `M` | Toggle mute |
| F12 | Save a screenshot |

The side panel provides mouse controls for the lever, button, pitch, roll, single and double taps, steps, battery voltage, USB connection, and charging.

## Hardware revisions

The two hardware revisions use different displays. Revision 2 shipped with firmware 2.0. Revision 3 requires firmware 2.1 or later and rejects 2.0. The emulator defaults to revision 3; `device info` reports a connected watch's revision.

## With a real watch

Connect the watch over USB. Back up its flash before changing the firmware:

```sh
artemis-emu device info                        # chip, hardware revision, and firmware
artemis-emu device backup my-watch.bin         # save the full flash
artemis-emu device flash micropython           # install this project's MicroPython build
artemis-emu python examples/hello.py --device  # run a program
artemis-emu repl --device
artemis-emu device monitor --reset             # restart and show the serial console
artemis-emu device flash stock --version 2.0   # install a stock release
artemis-emu device restore my-watch.bin        # restore the backup
```

`device flash` also accepts a firmware file. Flashing MicroPython preserves an existing MicroPython filesystem; otherwise, it erases the filesystem region. Add `--erase-files` to start with an empty filesystem. Use `--port` to select a watch when several are connected.

The watch uses an Espressif USB Serial/JTAG device (`303a:1001`). To access its serial port, join the serial-port group and log in again:

```sh
sudo usermod -aG dialout $USER  # the group is uucp on Arch
```

On NixOS, grant access with a udev rule:

```nix
services.udev.packages = [
  (pkgs.writeTextDir "lib/udev/rules.d/70-esp-usb-jtag.rules" ''
    SUBSYSTEMS=="usb", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"
  '')
];
```

WSL requires [usbipd-win](https://github.com/dorssel/usbipd-win) to attach the USB device to Linux. This setup is untested with the emulator's device tools.

## Firmware

MicroPython is built from unmodified [CircuitMess source](https://github.com/CircuitMess/micropython) using their ESP32-S3 board definition. Files occupy a FAT partition in the upper half of flash. An installed `main.py` survives power cycles on hardware and in persistent emulator flash images. The CPU runs at 160 MHz by default.

Stock firmware is downloaded from [CircuitMess's releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) on first use. Firmware images are not stored in this repository.

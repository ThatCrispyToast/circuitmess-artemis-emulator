# Artemis Watch emulator

Runs real firmware for the [CircuitMess NASA Artemis Watch 2.0](https://circuitmess.com/products/nasa-artemis-watch-2-0)
on your computer, unmodified: a dual-core ESP32-S3 in QEMU, with the watch's display, lever, button,
LEDs, buzzer, motion sensor, clock and battery around it. Use it interactively, or drive it from
Python to test your code automatically. It has been checked against a real watch, down to how
long code takes and what the chip's registers read; the same tests run on both.

## Quick start

```sh
nix run . -- stock                      # CircuitMess's factory firmware
nix run . -- python examples/hello.py   # a MicroPython program
nix run . -- repl                       # a MicroPython prompt (Ctrl-] quits)
nix run . -- run firmware.bin           # a firmware you built
nix run .#test                          # this project's tests
```

The first run builds QEMU and MicroPython from source, which takes a few minutes. Everything is
pinned by hash; nothing is installed outside the Nix store.

`run` accepts a merged flash image, a bare application image (placed behind the stock firmware's
bootloader and partition table), or an ESP-IDF project or build directory. `stock` runs
CircuitMess's latest release; `--version 2.0`, `2.1`, `2.1.1` or `2.2` picks another.

Useful options, for any of these commands:

| Option | Effect |
| --- | --- |
| `--headless` | no window; a `python` run ends when the script does |
| `--timeout SECONDS` | stop after this long |
| `--screenshot FILE.png` | save the display on exit |
| `--flash FILE` | keep flash (settings, files, NVS) in a file between runs |
| `--time 2026-07-20T20:17:00` | set the clock; the default is your local time, and `unset` is a clock that was never set, as a new watch has |
| `--battery MV`, `--usb`, `--charging` | start with this power supply |
| `--revision 2` | emulate hardware revision 2 instead of 3 (see below) |
| `--turbo` | run code as fast as your computer can, instead of at the chip's speed |
| `--sensor-noise` | give the motion sensor the noise a real one has |
| `--mute` | don't play the buzzer |
| `--no-watchdogs` | keep watchdogs from firing, for debugging a hang |
| `--gdb PORT`, `--wait-gdb` | open a GDB stub, optionally waiting for it before starting |

`python` also takes `--install`, which stores the script as `main.py` so that it runs at every
boot, the way a program from CircuitBlocks does:

```sh
nix run . -- python my_program.py --install --flash my_watch.bin
nix run . -- repl --flash my_watch.bin   # boots into main.py; Ctrl-C for a prompt
```

### In the window

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

The mouse works on the lever, the button, the tilt pad, the battery slider, and the other controls
in the side panel: double tap, steps, USB cable and charging.

### Which hardware revision

Watches come in two revisions, with different displays. Revision 2 shipped with firmware 2.0;
revision 3 needs 2.1 or later, and refuses 2.0. The emulator defaults to 3. `device info` says
which a real watch is.

## With a real watch

Plug the watch in over USB and the same commands work on it:

```sh
nix run . -- device info                        # chip, revision, and what firmware it's running
nix run . -- device backup my-watch.bin         # save its whole flash, and do this first
nix run . -- device flash micropython           # put this project's MicroPython on it
nix run . -- python examples/hello.py --device  # run a program on the watch
nix run . -- repl --device
nix run . -- device monitor --reset             # watch it boot
nix run . -- device flash stock --version 2.0   # a CircuitMess release
nix run . -- device restore my-watch.bin        # exactly as it was
```

`device flash` also takes a firmware file. Flashing MicroPython keeps a MicroPython filesystem
that is already on the watch, and clears whatever another firmware left where it goes;
`--erase-files` starts clean either way. With several watches plugged in, `--port` picks one.

Your user needs access to the watch's serial port, which is an Espressif USB Serial/JTAG device
(`303a:1001`). On NixOS:

```nix
services.udev.packages = [
  (pkgs.writeTextDir "lib/udev/rules.d/70-esp-usb-jtag.rules" ''
    SUBSYSTEMS=="usb", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"
  '')
];
```

## Testing your code

`artemis_emu` lets a test act as the user and look at what the watch does:

```python
import os
from artemis_emu import PythonSession, Watch

FIRMWARE = os.environ["ARTEMIS_MICROPYTHON_FIRMWARE"]

def test_lever_push_lights_an_led():
    with Watch(FIRMWARE) as watch:
        session = PythonSession(watch)
        session.start(open("examples/hello.py").read())
        # Once it has drawn its text, it's reading the lever.
        watch.wait_until(lambda: (255, 255, 255) in watch.pixels())

        watch.press("select", 0.15)
        watch.wait_until(lambda: watch.leds[0] == 1.0, timeout=2)

        watch.set_button("alt", True)
        assert session.wait(timeout=5).strip() == "pushed 1 times"
```

Run it with `nix develop -c python -m pytest my_test.py`, which provides the emulator, firmware and
Python. `tests/test_examples.py` has more of this.

| `Watch` | |
| --- | --- |
| Inputs | `press(button, seconds)`, `set_button(button, pressed)`, `tilt(pitch, roll)`, `set_motion(accel_g, gyro_dps)`, `tap(axis, double)`, `steps(count)`, `set_battery(mv, usb, charging)`, `set_time(datetime)`, `reset()` |
| Outputs | `frame()`, `pixels()`, `pixel(x, y)`, `screenshot(path)`, `leds`, `rgb_led`, `backlight`, `buzzer`, `powered` |
| Serial | `usb_output`, `uart_output`, `wait_for_output(regex)`, `write_serial(text)` |
| Waiting | `wait_until(condition)`, `wait_until_display_settles()`, `wait_for_frame_change()` |
| Options | `revision`, `battery_mv`, `usb`, `charging`, `time`, `flash_file`, `watchdogs`, `turbo`, `sensor_noise`, `gdb_port`, `wait_for_gdb`, `qemu_args` |

Buttons are `up`/`lever_right`, `down`/`lever_left`, `select`/`lever_push` and `back`/`alt`/`button`.
Inputs have taken effect when these calls return, and outputs are as of the moment you read them.
The watch runs in its own time, as the chip would: a change your code asks for shows up when
the hardware gets to it (a new PWM duty at the end of the period, say), so wait for what you
expect rather than reading it straight after.

`PythonSession(watch)` talks to MicroPython's raw REPL. `run(code)` returns what the code printed,
and raises `PythonError` with the traceback if it fails or `WatchRestarted` if the watch reboots
first. `start(code)` and `wait()` split that in two, for interacting with a program while it runs.
`write_file(name, content)` stores a file, and `soft_reset()` restarts MicroPython.

`Device()` is a real watch on USB, and a `PythonSession` works on it the same way, so a test of
what your code prints or reads can run on either:

```python
from artemis_emu import Device, PythonSession

with Device() as watch:
    print(PythonSession(watch).run("import os; print(os.uname().machine)"))
```

The watch's library is on the MicroPython side as `Artemis`, the same as on the device:
`begin()`, `display`, `buttons`, `leds`, `rgb`, `backlight`, `piezo`, `imu`, `rtc`. See `examples/`.

## What's emulated

| Part | Status |
| --- | --- |
| ESP32-S3 | both cores, ROM, 4 MB flash, 2 MB PSRAM, at 20 to 240 MHz, running code at the chip's speed |
| Display | ST7735S over SPI with DMA, on both hardware revisions |
| Inputs and outputs | lever, side button, six LEDs, RGB LED, backlight and buzzer, including PWM and hardware fades |
| Motion sensor | LSM6DS3TR-C: accelerometer, gyroscope, FIFO, timestamps, interrupts; wrist tilt, tap, steps, free fall, wake-up and orientation |
| Clock | BM8563 real-time clock |
| Power | battery voltage through the ADC, USB and charging detection, the charger's status line, the power-off latch |
| Serial | USB Serial/JTAG console, UART0 |
| Sleep | light and deep sleep; wakeup by timer, EXT0, EXT1 and GPIO; RTC memory is kept |
| Watchdogs | task, interrupt and RTC watchdogs, with panics and restarts as on the chip |
| Storage | flash, NVS, and MicroPython's filesystem |
| Other parts of the chip | RMT (NeoPixels), I2S, touch pads, timers, the ADC, and UART, SPI and I2C ports with nothing on them: none is wired to anything on the watch, but code that uses them runs as it does there |
| Bluetooth | the real controller runs: advertising, scanning and GATT servers work and connection attempts time out, but nothing goes on the air |
| Wi-Fi | the stack starts in both modes; scans find nothing and connecting never succeeds. Turning it off takes five seconds, where a watch takes milliseconds |

## How close it is to a real watch

`tests/test_like_a_watch.py` holds what was measured on a watch (hardware revision 2, running
this project's MicroPython) and requires the emulator to do the same. With a watch plugged in,
`ARTEMIS_TEST_ON_WATCH=1` runs those very tests on it instead, which is how the figures in them
were checked.

- **Speed.** Code takes the time it takes on the chip, at any CPU frequency. Of 27 MicroPython
  operations timed on both, 17 are within 3% and all within 11%; six longer workloads are
  within 3% to 8%. Work that leans on memory and flash, such as compiling Python source or
  building strings, is the furthest out, up to 15% fast. The first run of a piece of code is
  no slower than the next. Reading or writing a peripheral's register costs what it does on
  the chip, RTC registers more than the rest.
- **Time.** Sleeps, timers, PWM, UART and sensor data rates keep the chip's timing, and
  clocks that code can read advance with the instructions it runs. An idle core wakes for
  its tick within 5 microseconds of when it is due as a rule and 60 at worst, where a
  watch manages 1 and 23.
- **Registers.** Of 2004 registers that read the same on a watch from one reset to the next,
  1994 read the same here once MicroPython has started. The other ten are the flash
  controller's, and are listed in the test. A peripheral without its clock, or held in reset,
  ignores writes as it does on the chip.
- **Resets.** Each kind of reset leaves what it leaves on a watch: a software or watchdog reset
  keeps the RTC domain, with its clock and scratch registers, and a whole-chip reset doesn't.

Keeping that timing costs about one core of your computer while the watch is busy, and a
fifth of one while it idles.

What a test on the emulator can't tell you:

- **Radio traffic.** There's nothing to pair with or connect to, so phone pairing can't be tested.
- **Cache misses.** Code and data in flash or PSRAM are as fast as when cached, which is why
  memory-heavy work runs a little fast.
- **Analogue behaviour.** The ADC reads ideal voltages through a real chip's calibration. LEDs
  and the backlight are shown by duty cycle, the buzzer as an ideal square wave, and nothing
  draws current.
- **Parts with no model.** The ULP coprocessor, USB OTG, TWAI, motor PWM, SD host, camera and LCD
  interfaces keep their register values and do nothing.
- **The exact instruction an interrupt lands on.** It can be a microsecond or two off, which
  only shows where firmware is sensitive to it: the interrupt watchdog's panic reads
  "Unhandled debug exception" one time in thirty here, and never on the watch.
- **Other watches.** One watch was measured. Another's oscillators, sensor offsets and ADC will
  differ a little; the motion sensor of the one measured runs 4.5% fast, where the emulator's
  is exact.

## Firmware

The MicroPython firmware is built from [CircuitMess's source](https://github.com/CircuitMess/micropython)
for the board they maintain for their ESP32-S3 devices, unchanged. It keeps files in a FAT
partition in the upper half of flash, so a program installed as `main.py` survives a power
cycle, here and on a watch. Its CPU runs at ESP-IDF's default 160 MHz.

The stock firmware is downloaded from
[CircuitMess's releases](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) when
needed, not kept here.

## How it's put together

```
flake.nix               packages, apps and the development shell
nix/qemu.nix            Espressif's QEMU, plus the two directories below
nix/micropython.nix     CircuitMess's MicroPython, with its pinned toolchain and ESP-IDF
qemu/overlay/           the watch's devices and the chip's missing ones, added to QEMU's source tree
qemu/patches/           wires them into the ESP32-S3 machine, paces the CPUs, and fixes the model where it differs from the chip
qemu/tools/             generates the table of register reset values from ESP-IDF's headers, and unpacks the patch for editing
artemis_emu/            command line, window, real-watch support, and the Python API
examples/               MicroPython programs
tests/                  runs the stock firmware, MicroPython, the examples and the command line; test_like_a_watch.py runs on a watch too
```

QEMU and the front end talk over a small protocol on a Unix socket, documented at the top of
`qemu/overlay/hw/artemis/board.c`. The serial ports and QMP are Unix sockets too, in a temporary
directory per run.

The CPUs are paced by counting instructions: each core runs a few microseconds' worth, then
waits for the clock. The clock stands still while QEMU translates code or runs a device model,
since neither takes the chip any time. `--turbo` turns all of that off.

The patch to QEMU's own files is a diff, and is edited as the two trees it is the difference
of: `qemu/tools/patch-trees.sh unpack DIR`, change files under `DIR/b`, then `pack DIR`.

Flakes only see files tracked by Git, so `git add` new files before building.

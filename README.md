# Artemis Watch emulator

Runs real firmware for the [CircuitMess NASA Artemis Watch 2.0](https://circuitmess.com/products/nasa-artemis-watch-2-0)
on your computer, unmodified: a dual-core ESP32-S3 in QEMU, with the watch's display, lever, button,
LEDs, buzzer, motion sensor, clock and battery around it. Use it interactively, or drive it from
Python to test your code automatically.

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
bootloader and partition table), or an ESP-IDF project or build directory.

Useful options, for any of these commands:

| Option | Effect |
| --- | --- |
| `--headless` | no window; a `python` run ends when the script does |
| `--timeout SECONDS` | stop after this long |
| `--screenshot FILE.png` | save the display on exit |
| `--flash FILE` | keep flash (settings, files, NVS) in a file between runs |
| `--time 2026-07-20T20:17:00` | set the clock; the default is your local time |
| `--battery MV`, `--usb`, `--charging` | start with this power supply |
| `--revision 2` | emulate hardware revision 2 instead of 3 |
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
| Options | `revision`, `battery_mv`, `usb`, `charging`, `time`, `flash_file`, `watchdogs`, `gdb_port`, `wait_for_gdb`, `qemu_args` |

Buttons are `up`/`lever_right`, `down`/`lever_left`, `select`/`lever_push` and `back`/`alt`/`button`.
Inputs have taken effect when these calls return, and outputs are as of the moment you read them.

`PythonSession(watch)` talks to MicroPython's raw REPL. `run(code)` returns what the code printed,
and raises `PythonError` with the traceback if it fails or `WatchRestarted` if the watch reboots
first. `start(code)` and `wait()` split that in two, for interacting with a program while it runs.
`write_file(name, content)` stores a file, and `soft_reset()` restarts MicroPython.

The watch's library is on the MicroPython side as `Artemis`, the same as on the device:
`begin()`, `display`, `buttons`, `leds`, `rgb`, `backlight`, `piezo`, `imu`, `rtc`. See `examples/`.

## What's emulated

| Part | Status |
| --- | --- |
| ESP32-S3 | both cores, ROM, 4 MB flash, 2 MB PSRAM, at 20 to 240 MHz |
| Display | ST7735S over SPI with DMA, on both hardware revisions |
| Inputs and outputs | lever, side button, six LEDs, RGB LED, backlight and buzzer, including PWM and hardware fades |
| Motion sensor | LSM6DS3TR-C: accelerometer, gyroscope, FIFO, wrist tilt, tap, step counter |
| Clock | BM8563 real-time clock |
| Power | battery voltage through the ADC, USB and charging detection, the power-off latch |
| Serial | USB Serial/JTAG console, UART0 |
| Sleep | light and deep sleep; wakeup by timer, EXT0, EXT1 and GPIO; RTC memory is kept |
| Watchdogs | task and interrupt watchdogs, with panics and restarts as on the chip |
| Storage | flash, NVS, and MicroPython's filesystem |
| Bluetooth | the real controller runs: advertising, scanning and GATT servers work and connection attempts time out, but nothing goes on the air |
| Wi-Fi | the stack starts in both modes; scans find nothing and connecting never succeeds |

## What isn't

- **Radio traffic.** There's nothing to pair with or connect to, so phone pairing can't be tested.
- **Timing of your code.** Timers follow real time, but instructions take as long as QEMU takes to
  run them, not as long as the chip would. CPU-bound work won't take the same time as on a watch.
- **Parts the watch doesn't use.** I2S hangs when written to, NeoPixels fail with an error, touch
  pads read 0, and other I2C, SPI and UART ports connect to nothing. The ULP and the RTC watchdog
  aren't modelled.
- **Some motion events.** Free-fall, wake-up and 6D flags always read as inactive.
- **The look of light.** LEDs and the backlight are shown by duty cycle, and the buzzer as an
  ideal square wave.

Revision 2 boots both firmwares and draws correctly, but has less test coverage than revision 3.

## Where it differs from a real watch

The MicroPython firmware is built from
[CircuitMess's source](https://github.com/CircuitMess/micropython), with one deliberate change. Its
published partition table has nothing MicroPython would mount as a filesystem, yet CircuitMess
[documents](https://help.circuitmess.com/en/articles/14828483-what-happens-to-my-circuitblocks-program-when-the-device-resets)
that a program is saved to the device and restarts after a power cycle. The image CircuitBlocks
flashes isn't public, so its mechanism is unknown; `nix/micropython.nix` renames the data partition
to `vfs` to get the documented behaviour. Code that writes files works here, but check it on a
real watch too.

Its CPU runs at ESP-IDF's default 160 MHz, since the published configuration doesn't raise it.

## How it's put together

```
flake.nix               packages, apps and the development shell
nix/qemu.nix            Espressif's QEMU, plus the two directories below
nix/micropython.nix     CircuitMess's MicroPython, with its pinned toolchain and ESP-IDF
qemu/overlay/           the watch's devices, added to QEMU's source tree
qemu/patches/           wires them into the ESP32-S3 machine, and fixes the model where it differs from the chip
artemis_emu/            command line, window, and the Python API
examples/               MicroPython programs
tests/                  runs the stock firmware, MicroPython, the examples and the command line
```

QEMU and the front end talk over a small protocol on a Unix socket, documented at the top of
`qemu/overlay/hw/artemis/board.c`. The serial ports and QMP are Unix sockets too, in a temporary
directory per run.

The stock firmware is downloaded from
[CircuitMess's release](https://github.com/CircuitMess/GC_Artemis-Firmware/releases) when needed,
not kept here.

Flakes only see files tracked by Git, so `git add` new files before building.

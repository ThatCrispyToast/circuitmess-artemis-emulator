# Testing your code

Use `artemis_emu` to control inputs and inspect the display, LEDs, buzzer, and serial output:

```python
from artemis_emu import PythonSession, Watch, micropython_firmware

def test_lever_push_lights_an_led():
    with Watch(micropython_firmware()) as watch:
        session = PythonSession(watch)
        session.start(open("examples/hello.py").read())
        # Wait for the program to display its text before pressing the lever.
        watch.wait_until(lambda: (255, 255, 255) in watch.pixels())

        watch.press("select", 0.15)
        watch.wait_until(lambda: watch.leds[0] == 1.0, timeout=2)

        watch.set_button("alt", True)
        assert session.wait(timeout=5).strip() == "pushed 1 times"
```

Install pytest with `pip install '.[test]'`, then run the test with `python -m pytest my_test.py`. `micropython_firmware()` and `stock_firmware()` return firmware image paths, downloading the images if needed. See [`tests/test_examples.py`](../tests/test_examples.py) for more examples.

## `Watch`

| Category | API |
| --- | --- |
| Inputs | `press(button, seconds)`, `set_button(button, pressed)`, `tilt(pitch, roll)`, `set_motion(accel_g, gyro_dps)`, `tap(axis, double)`, `steps(count)`, `set_battery(mv, usb, charging)`, `set_time(datetime)`, `reset()` |
| Outputs | `frame()`, `pixels()`, `pixel(x, y)`, `screenshot(path)`, `leds`, `rgb_led`, `backlight`, `buzzer`, `powered` |
| Serial | `usb_output`, `uart_output`, `wait_for_output(regex)`, `write_serial(text)` |
| Waiting | `wait_until(condition)`, `wait_until_display_settles()`, `wait_for_frame_change()` |
| Options | `revision`, `battery_mv`, `usb`, `charging`, `time`, `flash_file`, `watchdogs`, `turbo`, `sensor_noise`, `gdb_port`, `wait_for_gdb`, `qemu_args` |

Button aliases are `up`/`lever_right`, `down`/`lever_left`, `select`/`lever_push`, and `back`/`alt`/`button`.

Input calls return after the emulator applies the input; output reads return the current state. Firmware responses and peripheral updates take time. For example, a PWM duty change takes effect at the end of a period. Use `wait_until()` to wait for the expected output.

## `PythonSession`

`PythonSession(watch)` uses MicroPython's raw REPL. `run(code)` returns printed output, raises `PythonError` with a traceback on failure, or raises `WatchRestarted` if the watch reboots during execution.

Use `start(code)` and `wait()` to interact with a program while it runs. `write_file(name, content)` saves a file, and `soft_reset()` restarts MicroPython.

MicroPython programs access the watch through CircuitMess's `Artemis` library: `begin()`, `display`, `buttons`, `leds`, `rgb`, `backlight`, `piezo`, `imu`, and `rtc`. See [`examples/`](../examples).

## On a real watch

`Device()` connects to a watch over USB. `PythonSession` accepts either a `Watch` or a `Device`, allowing tests of program input and output to run on both:

```python
from artemis_emu import Device, PythonSession

with Device() as watch:
    print(PythonSession(watch).run("import os; print(os.uname().machine)"))
```

[`tests/test_hardware_comparison.py`](../tests/test_hardware_comparison.py) checks hardware measurements and runs on the emulator by default. To run it on a watch, install this project's MicroPython and set `ARTEMIS_TEST_ON_WATCH=1`:

```sh
artemis-emu device flash micropython
ARTEMIS_TEST_ON_WATCH=1 python -m pytest tests/test_hardware_comparison.py
```

## Project checks

Run `python -m pytest -q tests` for the full suite. Hardware timing comparisons require native Linux; virtual machines can exceed their tolerances, as described in [the accuracy guide](accuracy.md).

GitHub Actions checks package installation, first-use downloads, the GUI under Xvfb, and the functional suite on Ubuntu 22.04 / Python 3.10 and Ubuntu 24.04 / Python 3.12. It excludes `test_hardware_comparison.py` because the hosted runners are virtual machines. Release validation runs the full suite on native Linux.

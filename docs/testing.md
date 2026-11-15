# Testing your code

`artemis_emu` lets a test act as the user and look at what the watch does:

```python
from artemis_emu import PythonSession, Watch, micropython_firmware

def test_lever_push_lights_an_led():
    with Watch(micropython_firmware()) as watch:
        session = PythonSession(watch)
        session.start(open("examples/hello.py").read())
        # Once it has drawn its text, it's reading the lever.
        watch.wait_until(lambda: (255, 255, 255) in watch.pixels())

        watch.press("select", 0.15)
        watch.wait_until(lambda: watch.leds[0] == 1.0, timeout=2)

        watch.set_button("alt", True)
        assert session.wait(timeout=5).strip() == "pushed 1 times"
```

Run it with `python -m pytest my_test.py`, once `pip install '.[test]'` has added pytest to the emulator's environment. `micropython_firmware()` and `stock_firmware()` give the images the emulator comes with. `tests/test_examples.py` has more of this.

## `Watch`

| | |
| --- | --- |
| Inputs | `press(button, seconds)`, `set_button(button, pressed)`, `tilt(pitch, roll)`, `set_motion(accel_g, gyro_dps)`, `tap(axis, double)`, `steps(count)`, `set_battery(mv, usb, charging)`, `set_time(datetime)`, `reset()` |
| Outputs | `frame()`, `pixels()`, `pixel(x, y)`, `screenshot(path)`, `leds`, `rgb_led`, `backlight`, `buzzer`, `powered` |
| Serial | `usb_output`, `uart_output`, `wait_for_output(regex)`, `write_serial(text)` |
| Waiting | `wait_until(condition)`, `wait_until_display_settles()`, `wait_for_frame_change()` |
| Options | `revision`, `battery_mv`, `usb`, `charging`, `time`, `flash_file`, `watchdogs`, `turbo`, `sensor_noise`, `gdb_port`, `wait_for_gdb`, `qemu_args` |

Buttons are `up`/`lever_right`, `down`/`lever_left`, `select`/`lever_push` and `back`/`alt`/`button`. Inputs have taken effect when these calls return, and outputs are as of the moment you read them. The watch runs in its own time, as the chip would: a change your code asks for shows up when the hardware gets to it (a new PWM duty at the end of the period, say), so wait for what you expect rather than reading it straight after.

## `PythonSession`

`PythonSession(watch)` talks to MicroPython's raw REPL. `run(code)` returns what the code printed, and raises `PythonError` with the traceback if it fails or `WatchRestarted` if the watch reboots first. `start(code)` and `wait()` split that in two, for interacting with a program while it runs. `write_file(name, content)` stores a file, and `soft_reset()` restarts MicroPython.

The watch's library is on the MicroPython side as `Artemis`, the same as on the device: `begin()`, `display`, `buttons`, `leds`, `rgb`, `backlight`, `piezo`, `imu`, `rtc`. See `examples/`.

## On a real watch

`Device()` is a real watch on USB, and a `PythonSession` works on it the same way, so a test of what your code prints or reads can run on either:

```python
from artemis_emu import Device, PythonSession

with Device() as watch:
    print(PythonSession(watch).run("import os; print(os.uname().machine)"))
```

`tests/test_like_a_watch.py` is written this way. It holds what was measured on a watch, and runs on the emulator by default or on a watch with this project's MicroPython on it:

```sh
artemis-emu device flash micropython
ARTEMIS_TEST_ON_WATCH=1 python -m pytest tests/test_like_a_watch.py
```

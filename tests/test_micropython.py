"""Runs Python on CircuitMess's MicroPython firmware against the emulated hardware."""

import datetime as dt
import os
import re
import threading
import time

import pytest

from artemis_emu import PythonError, PythonSession, Watch, WatchRestarted

FIRMWARE = os.environ.get("ARTEMIS_MICROPYTHON_FIRMWARE")
SETUP = "import Artemis, machine, time\nArtemis.begin()\n"


def evaluate(session, expression):
    return eval(session.run(f"print(repr({expression}))").strip())


def reboot_detector(watch):
    boots = watch.usb_output.count("ESP-ROM:esp32s3")
    return lambda: watch.usb_output.count("ESP-ROM:esp32s3") > boots


# ----------------------------------------------------------------------
# The interpreter and the link to it


def test_prints_output(micropython):
    assert micropython.run("print(6 * 7)").strip() == "42"


def test_reports_exceptions_with_a_traceback(micropython):
    with pytest.raises(PythonError) as error:
        micropython.run("def f():\n    raise ValueError('nope')\nf()")
    assert "ValueError: nope" in error.value.traceback
    assert "in f" in error.value.traceback


def test_session_survives_an_exception(micropython):
    with pytest.raises(PythonError):
        micropython.run("1 / 0")
    assert micropython.run("print('still here')").strip() == "still here"


def test_is_the_circuitmess_build_for_their_s3_devices(micropython):
    machine = evaluate(micropython, "__import__('os').uname().machine")
    assert machine == "CircuitMess ESP32S3 Devices with ESP32S3"
    assert evaluate(micropython, "__import__('efuse').read_rev()") == 3
    assert evaluate(micropython, "__import__('efuse').read_pid()") == 3


def test_a_big_script_survives_the_serial_link(micropython):
    # Longer than any buffer between here and the interpreter.
    lines = "\n".join(f"v{i} = {i}" for i in range(2000))
    assert micropython.run(lines + "\nprint(v1999)").strip() == "1999"


def test_large_output_is_not_lost(micropython):
    out = micropython.run("for i in range(5000):\n    print(i)")
    assert out.split() == [str(i) for i in range(5000)]


def test_script_timeout_interrupts_it(micropython):
    with pytest.raises(TimeoutError):
        micropython.run("while True:\n    pass", timeout=1)
    session = PythonSession(micropython.watch)
    assert session.run("print('recovered')").strip() == "recovered"


def test_script_that_restarts_the_watch_is_reported(micropython):
    with pytest.raises(WatchRestarted):
        micropython.run("import machine\nmachine.reset()")


def test_soft_reset_forgets_variables_and_keeps_the_session(micropython):
    micropython.run("forget_me = 1\nimport machine\nmachine.soft_reset()")
    assert evaluate(micropython, "'forget_me' in globals()") is False
    assert evaluate(micropython, "__import__('machine').reset_cause() == __import__('machine').SOFT_RESET")


# ----------------------------------------------------------------------
# Timing


def test_cpu_defaults_to_160_mhz(micropython):
    # The published build leaves ESP-IDF's default in place.
    assert evaluate(micropython, "__import__('machine').freq()") == 160_000_000


@pytest.mark.parametrize("mhz", [20, 40, 80, 160, 240])
def test_cpu_speed_can_be_changed_and_back(micropython, mhz):
    micropython.run(f"import machine\nmachine.freq({mhz}_000_000)", timeout=20)
    assert evaluate(micropython, "machine.freq()") == mhz * 1_000_000
    micropython.run("machine.freq(160_000_000)", timeout=20)
    assert evaluate(micropython, "machine.freq()") == 160_000_000


def test_sleep_takes_as_long_as_it_says(micropython):
    micropython.run("import time\nstart = time.ticks_ms()\ntime.sleep_ms(500)\nend = time.ticks_ms()")
    assert 495 <= evaluate(micropython, "time.ticks_diff(end, start)") <= 560


@pytest.mark.parametrize("mhz", [80, 240])
def test_clock_keeps_up_after_changing_cpu_speed(micropython, mhz):
    micropython.run(
        f"""
import machine, time
machine.freq({mhz}_000_000)
start = time.ticks_ms()
time.sleep_ms(500)
end = time.ticks_ms()
"""
    )
    assert 495 <= evaluate(micropython, "time.ticks_diff(end, start)") <= 560


def test_hardware_timer_fires_periodically(micropython):
    micropython.run(
        """
from machine import Timer
import time
ticks = []
timer = Timer(0)
timer.init(period=50, mode=Timer.PERIODIC, callback=lambda t: ticks.append(1))
time.sleep_ms(525)
timer.deinit()
"""
    )
    assert evaluate(micropython, "len(ticks)") in (9, 10, 11)


def test_threads_run(micropython):
    micropython.run(
        """
import _thread, time
done = []
_thread.start_new_thread(lambda: done.append(1), ())
time.sleep_ms(50)
"""
    )
    assert evaluate(micropython, "len(done)") == 1


def test_asyncio_runs(micropython):
    out = micropython.run(
        """
import uasyncio as asyncio

async def worker(name, delay):
    await asyncio.sleep_ms(delay)
    print(name)

async def main():
    await asyncio.gather(worker("slow", 100), worker("fast", 10))

asyncio.run(main())
"""
    )
    assert out.split() == ["fast", "slow"]


# ----------------------------------------------------------------------
# Display


def test_begin_turns_on_a_black_display(micropython):
    micropython.run(SETUP)
    watch = micropython.watch
    assert watch.backlight == 1.0
    assert set(watch.pixels()) == {(0, 0, 0)}


def test_display_shows_what_was_drawn_the_right_way_up(micropython):
    micropython.run(
        SETUP
        + """
d = Artemis.display
d.fill(d.Color.Black)
d.fill_rect(0, 0, 10, 20, d.Color.Red)
d.fill_rect(118, 118, 10, 10, d.Color.Blue)
d.pixel(64, 64, d.Color.White)
d.commit()
"""
    )
    pixels = micropython.watch.pixels()
    at = lambda x, y: pixels[y * 128 + x]
    assert at(0, 0) == (255, 0, 0)
    assert at(9, 19) == (255, 0, 0)
    assert at(10, 19) == (0, 0, 0)
    assert at(9, 20) == (0, 0, 0)
    assert at(127, 127) == (0, 0, 255)
    assert at(64, 64) == (255, 255, 255)
    assert at(127, 0) == (0, 0, 0)


def test_display_is_upright_on_revision_2():
    with Watch(FIRMWARE, revision=2) as watch:
        session = PythonSession(watch)
        session.run(SETUP + "d = Artemis.display\nd.fill_rect(0, 0, 10, 20, d.Color.Red)\nd.commit()")
        assert evaluate(session, "Artemis.revision") == 2
        pixels = watch.pixels()
        assert pixels[0] == (255, 0, 0)
        assert pixels[19 * 128 + 9] == (255, 0, 0)
        assert pixels[20 * 128 + 9] == (0, 0, 0)
        assert pixels[19 * 128 + 10] == (0, 0, 0)


def test_text_reaches_the_display(micropython):
    micropython.run(SETUP + "d = Artemis.display\nd.text('Artemis', 10, 60, d.Color.White)\nd.commit()")
    lit = sum(1 for p in micropython.watch.pixels() if p == (255, 255, 255))
    assert lit > 50


def test_backlight_follows_its_signal(micropython):
    micropython.run(SETUP)
    micropython.run("Artemis.backlight.off()")
    assert micropython.watch.backlight == 0.0
    micropython.run("Artemis.backlight.on()")
    assert micropython.watch.backlight == 1.0


# ----------------------------------------------------------------------
# Buttons


@pytest.mark.parametrize(
    "name, index",
    [("lever_right", 0), ("lever_left", 1), ("lever_push", 2), ("alt", 3)],
)
def test_each_button_reads_as_pressed(micropython, name, index):
    micropython.run(SETUP)
    watch = micropython.watch
    state = f"(Artemis.buttons.scan(), Artemis.buttons.state({index}))[1]"
    assert evaluate(micropython, state) is False
    watch.set_button(name, True)
    assert evaluate(micropython, state) is True
    watch.set_button(name, False)
    assert evaluate(micropython, state) is False


def test_button_press_callback_runs(micropython):
    micropython.run(SETUP + "presses = []\nArtemis.buttons.on_press(2, lambda: presses.append(1))")
    micropython.watch.set_button("select", True)
    assert evaluate(micropython, "(Artemis.buttons.scan(), len(presses))[1]") == 1


def test_pin_interrupt_fires_on_a_button(micropython):
    micropython.run(
        """
from machine import Pin
fired = []
pin = Pin(21, Pin.IN, Pin.PULL_DOWN)
pin.irq(lambda p: fired.append(1), Pin.IRQ_RISING)
"""
    )
    micropython.watch.press("alt", 0.05)
    time.sleep(0.1)
    assert evaluate(micropython, "len(fired)") == 1


# ----------------------------------------------------------------------
# LEDs and buzzer


def test_leds_light_one_at_a_time(micropython):
    micropython.run(SETUP)
    watch = micropython.watch
    for i in range(6):
        micropython.run(f"Artemis.leds[{i}].on()")
        assert watch.leds == [1.0 if n == i else 0.0 for n in range(6)]
        micropython.run(f"Artemis.leds[{i}].off()")
        assert watch.leds == [0.0] * 6


def test_led_brightness_follows_pwm(micropython):
    micropython.run("from machine import Pin, PWM\npwm = PWM(Pin(46), freq=1000, duty_u16=16384)")
    assert micropython.watch.leds[0] == pytest.approx(0.25, abs=0.01)


def test_rgb_led_mixes_channels(micropython):
    watch = micropython.watch
    micropython.run(SETUP + "Artemis.rgb.set(100, 50, 0)")
    # A new duty takes effect at the end of the PWM period, a millisecond here.
    watch.wait_until(lambda: watch.rgb_led[0] > 0.99, timeout=1)
    r, g, b = watch.rgb_led
    # The driver squares brightness for a linear response to the eye.
    assert g == pytest.approx(0.25, abs=0.02)
    assert b < 0.01


def test_buzzer_plays_the_requested_tone(micropython):
    watch = micropython.watch
    micropython.run(SETUP + "pwm = machine.PWM(machine.Pin(47), freq=1760, duty_u16=32768)")
    freq, duty = watch.buzzer
    assert abs(freq - 1760) <= 1
    assert duty == pytest.approx(0.5, abs=0.01)
    micropython.run("pwm.deinit()")
    assert watch.buzzer[0] == 0


def test_piezo_tone_stops_on_its_own(micropython):
    watch = micropython.watch
    micropython.run(SETUP)
    seen = set()
    runner = threading.Thread(target=micropython.run, args=("Artemis.piezo.tone(880, 300)",))
    runner.start()
    while runner.is_alive():
        seen.add(watch.buzzer[0])
        time.sleep(0.005)
    runner.join()
    assert any(abs(f - 880) <= 1 for f in seen)
    assert watch.buzzer[0] == 0


# ----------------------------------------------------------------------
# Motion sensor and clock


def test_i2c_bus_has_both_chips(micropython):
    micropython.run(SETUP)
    assert set(evaluate(micropython, "Artemis.i2c.scan()")) == {0x51, 0x6A}


def test_accelerometer_reads_the_tilt(micropython):
    micropython.run(SETUP)
    x, y, z = evaluate(micropython, "Artemis.imu.get_accel()")
    assert (x, y) == (pytest.approx(0, abs=0.02), pytest.approx(0, abs=0.02))
    assert z == pytest.approx(1, abs=0.02)

    micropython.watch.set_motion(accel_g=(0.5, -0.25, 0.75))
    x, y, z = evaluate(micropython, "Artemis.imu.get_accel()")
    assert x == pytest.approx(0.5, abs=0.02)
    assert y == pytest.approx(-0.25, abs=0.02)
    assert z == pytest.approx(0.75, abs=0.02)


def test_gyroscope_reads_the_rotation(micropython):
    micropython.run(SETUP)
    micropython.watch.set_motion(gyro_dps=(90.0, 0.0, -180.0))
    x, y, z = evaluate(micropython, "Artemis.imu.get_gyro()")
    assert x == pytest.approx(1.5708, abs=0.1)  # the library returns radians per second
    assert y == pytest.approx(0, abs=0.1)
    assert z == pytest.approx(-3.1416, abs=0.1)


def test_rtc_reads_the_time_it_was_given(micropython):
    micropython.run(SETUP + "t = Artemis.rtc.get_time()")
    assert evaluate(micropython, "(t.year, t.month, t.day, t.hours, t.minutes)") == (
        2026, 7, 20, 20, 17,
    )


def test_rtc_keeps_time_set_from_python(micropython):
    micropython.run(
        SETUP
        + """
t = Artemis.rtc.get_time()
t.year, t.month, t.day, t.hours, t.minutes, t.seconds = 2031, 12, 25, 7, 41, 5
Artemis.rtc.set_time(t)
time.sleep_ms(2100)
t = Artemis.rtc.get_time()
"""
    )
    assert evaluate(micropython, "(t.year, t.month, t.day, t.hours, t.minutes)") == (
        2031, 12, 25, 7, 41,
    )
    assert evaluate(micropython, "t.seconds") in (7, 8)


# ----------------------------------------------------------------------
# Power


def battery_read_setup(session):
    session.run(
        """
from machine import ADC, Pin
adc = ADC(Pin(5), atten=ADC.ATTN_11DB)
read = lambda: sum(adc.read_uv() for _ in range(16)) // 16
"""
    )


def test_battery_voltage_reaches_the_adc(micropython):
    battery_read_setup(micropython)
    # The firmware's battery driver: 1:4 divider, 75 mV correction.
    assert evaluate(micropython, "read()") == pytest.approx((3900 - 75) / 4 * 1000, rel=0.02)
    micropython.watch.set_battery(mv=3400)
    assert evaluate(micropython, "read()") == pytest.approx((3400 - 75) / 4 * 1000, rel=0.02)


def test_calibration_pin_switches_to_the_reference(micropython):
    battery_read_setup(micropython)
    micropython.run("cal = Pin(4, Pin.OUT, value=1)")
    assert evaluate(micropython, "read()") == pytest.approx(2500 / 4 * 1000, rel=0.02)


def test_usb_detect_pin_follows_the_cable(micropython):
    micropython.run("from machine import Pin\nusb = Pin(42, Pin.IN)")
    assert evaluate(micropython, "usb.value()") == 0
    micropython.watch.set_battery(usb=True)
    assert evaluate(micropython, "usb.value()") == 1


def test_power_pin_cuts_power_and_the_button_restores_it(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start("from machine import Pin\nPin(37, Pin.OUT, value=0)")
    watch.wait_until(lambda: not watch.powered, timeout=5)
    time.sleep(0.5)
    assert not rebooted()

    watch.press("alt", 0.2)
    watch.wait_until(rebooted, timeout=5)
    assert watch.powered
    assert PythonSession(watch).run("print('back')").strip() == "back"


# ----------------------------------------------------------------------
# Restarts, sleep and watchdogs


def test_machine_reset_reboots(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start("import machine\nmachine.reset()")
    watch.wait_until(rebooted, timeout=5)
    session = PythonSession(watch)
    assert evaluate(session, "__import__('machine').reset_cause() == __import__('machine').HARD_RESET")


def test_panic_reboots_and_says_why(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start("import machine\nmachine.mem32[0]")
    watch.wait_until(rebooted, timeout=10)
    assert "Guru Meditation Error" in watch.usb_output
    assert "LoadProhibited" in watch.usb_output
    assert PythonSession(watch).run("print('back')").strip() == "back"


def test_unfed_watchdog_restarts_the_watch(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.run("from machine import WDT\nw = WDT(timeout=1000)")
    start = time.monotonic()
    watch.wait_until(rebooted, timeout=5)
    assert time.monotonic() - start > 0.8
    assert "task_wdt" in watch.usb_output
    session = PythonSession(watch)
    assert evaluate(session, "__import__('machine').reset_cause() == __import__('machine').WDT_RESET")


def test_fed_watchdog_leaves_the_watch_running(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.run(
        """
from machine import WDT
import time
w = WDT(timeout=500)
for _ in range(20):
    time.sleep_ms(100)
    w.feed()
"""
    )
    assert not rebooted()


def test_watchdogs_can_be_turned_off():
    with Watch(FIRMWARE, watchdogs=False) as watch:
        session = PythonSession(watch)
        rebooted = reboot_detector(watch)
        session.run("from machine import WDT\nw = WDT(timeout=500)")
        time.sleep(2)
        assert not rebooted()


def test_interrupts_kept_off_trip_the_interrupt_watchdog(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start(
        """
import machine
machine.disable_irq()
while True:
    pass
"""
    )
    watch.wait_until(rebooted, timeout=10)
    # If the interrupt lands in a window vector, the panic handler's own spill faults first.
    assert re.search(r"panic'ed \((Interrupt wdt timeout|Unhandled debug exception)", watch.usb_output)
    assert PythonSession(watch).run("print('back')").strip() == "back"


def test_deep_sleep_wakes_on_its_timer(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start("import machine\nmachine.deepsleep(1000)")
    start = time.monotonic()
    watch.wait_until(rebooted, timeout=10)
    assert time.monotonic() - start >= 0.9
    session = PythonSession(watch)
    assert evaluate(session, "__import__('machine').reset_cause() == __import__('machine').DEEPSLEEP_RESET")


def test_rtc_memory_survives_deep_sleep(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start("import machine\nmachine.RTC().memory(b'remember')\nmachine.deepsleep(500)")
    watch.wait_until(rebooted, timeout=10)
    assert evaluate(PythonSession(watch), "__import__('machine').RTC().memory()") == b"remember"


def test_system_clock_survives_deep_sleep(micropython):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start(
        """
import machine
machine.RTC().datetime((2030, 1, 2, 2, 3, 4, 5, 0))
machine.deepsleep(1000)
"""
    )
    watch.wait_until(rebooted, timeout=10)
    year, month, day, _, hours, minutes, seconds, _ = evaluate(
        PythonSession(watch), "__import__('machine').RTC().datetime()"
    )
    assert (year, month, day, hours, minutes) == (2030, 1, 2, 3, 4)
    assert 5 <= seconds <= 12


@pytest.mark.parametrize("call", ["wake_on_ext0(Pin(21), esp32.WAKEUP_ANY_HIGH)",
                                  "wake_on_ext1([Pin(21)], esp32.WAKEUP_ANY_HIGH)"])
def test_button_wakes_from_deep_sleep(micropython, call):
    watch = micropython.watch
    rebooted = reboot_detector(watch)
    micropython.start(
        f"""
import esp32, machine
from machine import Pin
esp32.{call}
machine.deepsleep()
"""
    )
    time.sleep(1.5)
    assert not rebooted()
    watch.set_button("alt", True)
    watch.wait_until(rebooted, timeout=5)
    watch.set_button("alt", False)
    session = PythonSession(watch)
    assert evaluate(session, "__import__('machine').reset_cause() == __import__('machine').DEEPSLEEP_RESET")


def test_light_sleep_resumes_the_program(micropython):
    micropython.run(
        """
import machine, time
start = time.ticks_ms()
machine.lightsleep(500)
end = time.ticks_ms()
"""
    )
    assert evaluate(micropython, "time.ticks_diff(end, start)") >= 450


def test_button_wakes_from_light_sleep(micropython):
    micropython.run(
        """
import esp32, machine
from machine import Pin
button = Pin(21, Pin.IN, Pin.PULL_DOWN)
esp32.wake_on_ext0(button, esp32.WAKEUP_ANY_HIGH)
"""
    )
    micropython.start("machine.lightsleep()\nprint('awake')")
    time.sleep(1)
    assert b"awake" not in micropython.port.buffer
    micropython.watch.set_button("alt", True)
    assert micropython.wait(timeout=10).strip() == "awake"


def test_pin_interrupt_wakes_from_light_sleep(micropython):
    micropython.run(
        """
import machine
from machine import Pin
button = Pin(21, Pin.IN, Pin.PULL_DOWN)
button.irq(lambda p: None, Pin.WAKE_HIGH, wake=machine.SLEEP)
"""
    )
    micropython.start("machine.lightsleep()\nprint('awake')")
    time.sleep(1)
    assert b"awake" not in micropython.port.buffer
    micropython.watch.set_button("alt", True)
    assert micropython.wait(timeout=10).strip() == "awake"


# ----------------------------------------------------------------------
# Radios: the stacks start and find nobody


def test_wifi_can_be_turned_on_and_finds_nothing(micropython):
    out = micropython.run(
        """
import network
sta = network.WLAN(network.STA_IF)
sta.active(True)
print(sta.active(), sta.isconnected(), len(sta.scan()))
"""
    )
    assert out.split() == ["True", "False", "0"]


BLE_SETUP = """
import bluetooth, time
ble = bluetooth.BLE()
ble.active(True)
"""


def test_bluetooth_turns_on_and_off(micropython):
    assert micropython.run(BLE_SETUP + "print(ble.active())\nble.active(False)\nprint(ble.active())").split() == [
        "True", "False",
    ]


@pytest.mark.parametrize("delay_ms", [0, 5, 50, 300, 1000])
def test_advertising_stops_when_asked_whenever_that_is(micropython, delay_ms):
    out = micropython.run(
        BLE_SETUP
        + f"""
ble.gap_advertise(100_000, b"\\x02\\x01\\x06")
time.sleep_ms({delay_ms})
ble.gap_advertise(None)
ble.active(False)
print("stopped")
""",
        timeout=20,
    )
    assert out.strip() == "stopped"


def test_scanning_finishes_and_finds_nobody(micropython):
    out = micropython.run(
        BLE_SETUP
        + """
events = []
ble.irq(lambda event, data: events.append(event))
ble.gap_scan(500, 30000, 30000)
time.sleep_ms(1000)
print(5 in events, 6 in events)  # _IRQ_SCAN_RESULT, _IRQ_SCAN_DONE
""",
        timeout=20,
    )
    assert out.split() == ["False", "True"]


def test_connecting_to_nobody_times_out(micropython):
    out = micropython.run(
        BLE_SETUP
        + """
events = []
ble.irq(lambda event, data: events.append(event))
try:
    ble.gap_connect(0, b"\\x11\\x22\\x33\\x44\\x55\\x66", 500)
except OSError as e:
    print("refused", e)
time.sleep_ms(1500)
print(7 in events)  # _IRQ_PERIPHERAL_CONNECT
""",
        timeout=20,
    )
    assert out.split()[-1] == "False"


def test_a_gatt_server_can_be_registered(micropython):
    out = micropython.run(
        BLE_SETUP
        + """
from bluetooth import UUID, FLAG_READ
service = (UUID(0x180F), ((UUID(0x2A19), FLAG_READ),))
((level,),) = ble.gatts_register_services((service,))
ble.gatts_write(level, b"\\x64")
print(ble.gatts_read(level))
""",
        timeout=20,
    )
    assert out.strip() == "b'd'"


# ----------------------------------------------------------------------
# Storage


def test_nvs_stores_values(micropython):
    micropython.run(
        """
import esp32
nvs = esp32.NVS("emu")
nvs.set_i32("answer", 42)
nvs.commit()
"""
    )
    assert evaluate(micropython, "esp32.NVS('emu').get_i32('answer')") == 42


def test_filesystem_is_mounted_and_writable(micropython):
    micropython.write_file("note.txt", b"x" * 5000)
    assert evaluate(micropython, "__import__('os').stat('note.txt')[6]") == 5000
    assert evaluate(micropython, "len(open('note.txt').read())") == 5000


def test_installed_main_runs_at_boot(micropython):
    watch = micropython.watch
    micropython.write_file("main.py", b"print('hello from main.py')\n")
    micropython.soft_reset()
    watch.wait_for_output("hello from main.py", timeout=30)


def test_installed_main_runs_after_a_power_cycle(micropython):
    watch = micropython.watch
    micropython.write_file("main.py", b"print('hello from main.py')\n")
    assert "hello from main.py" not in watch.usb_output
    watch.reset()
    watch.wait_for_output("hello from main.py", timeout=30)


def test_files_outlive_the_emulator_with_a_flash_file(tmp_path):
    flash = tmp_path / "flash.bin"
    with Watch(FIRMWARE, flash_file=flash) as watch:
        PythonSession(watch).write_file("note.txt", b"remember me")

    with Watch(FIRMWARE, flash_file=flash) as watch:
        assert PythonSession(watch).run("print(open('note.txt').read())").strip() == "remember me"


def test_runs_with_a_set_clock_and_supply():
    when = dt.datetime(2030, 2, 3, 4, 5, 6)
    with Watch(FIRMWARE, time=when, battery_mv=3700, usb=True) as watch:
        session = PythonSession(watch)
        session.run(SETUP + "t = Artemis.rtc.get_time()")
        assert evaluate(session, "(t.year, t.month, t.day)") == (2030, 2, 3)
        session.run("from machine import ADC, Pin\nadc = ADC(Pin(5), atten=ADC.ATTN_11DB)\nusb = Pin(42, Pin.IN)")
        assert evaluate(session, "usb.value()") == 1
        # With a cable in, the battery divider sits on the USB rail, as on a real watch.
        assert evaluate(session, "adc.read_uv()") == pytest.approx(4370 / 4 * 1000, rel=0.03)

        watch.set_battery(usb=False)
        assert evaluate(session, "usb.value()") == 0
        assert evaluate(session, "adc.read_uv()") == pytest.approx((3700 - 75) / 4 * 1000, rel=0.03)

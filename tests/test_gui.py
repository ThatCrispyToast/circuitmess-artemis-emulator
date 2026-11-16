"""The window's controls, with Qt's offscreen platform standing in for a desktop."""

import os
import time

os.environ["QT_QPA_PLATFORM"] = "offscreen"

import pytest
from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QTest

from artemis_emu.gui import Buzzer, WatchWindow

SETUP = "import Artemis\nfrom machine import ADC, Pin\nArtemis.begin()\n"


@pytest.fixture
def window(micropython):
    micropython.run(SETUP)
    window = WatchWindow(micropython.watch, sound=False)
    window.show()
    yield window
    window.close()


def read(session, expression):
    return eval(session.run(f"print(repr({expression}))").strip())


def pin(session, number):
    return read(session, f"Pin({number}).value()")


def click(window, label):
    QTest.mouseClick(window.controls[label], Qt.MouseButton.LeftButton)


@pytest.mark.parametrize(
    "key, gpio",
    [
        (Qt.Key.Key_Left, 2),
        (Qt.Key.Key_Right, 1),
        (Qt.Key.Key_Return, 3),
        (Qt.Key.Key_Space, 3),
        (Qt.Key.Key_Escape, 21),
        (Qt.Key.Key_Backspace, 21),
    ],
)
def test_keyboard_controls_update_gpio(micropython, window, key, gpio):
    QTest.keyPress(window, key)
    assert pin(micropython, gpio) == 1
    QTest.keyRelease(window, key)
    assert pin(micropython, gpio) == 0


@pytest.mark.parametrize("label, gpio", [("Left", 2), ("Right", 1), ("Push", 3), ("Side button", 21)])
def test_mouse_controls_update_gpio(micropython, window, label, gpio):
    QTest.mousePress(window.controls[label], Qt.MouseButton.LeftButton)
    assert pin(micropython, gpio) == 1
    QTest.mouseRelease(window.controls[label], Qt.MouseButton.LeftButton)
    assert pin(micropython, gpio) == 0


def test_keyboard_controls_after_mouse_click(micropython, window):
    click(window, "Tap")
    QTest.keyPress(window, Qt.Key.Key_Space)
    assert pin(micropython, 3) == 1
    QTest.keyRelease(window, Qt.Key.Key_Space)


def test_tilt_keys_update_accelerometer(micropython, window):
    for _ in range(3):
        QTest.keyClick(window, Qt.Key.Key_W)
    _, y, _ = read(micropython, "Artemis.imu.get_accel()")
    assert y == pytest.approx(0.5, abs=0.02)
    assert window.pitch_slider.value() == 30

    QTest.keyClick(window, Qt.Key.Key_0)
    x, y, z = read(micropython, "Artemis.imu.get_accel()")
    assert (x, y, z) == (
        pytest.approx(0, abs=0.02),
        pytest.approx(0, abs=0.02),
        pytest.approx(1, abs=0.02),
    )


def test_tilt_sliders_update_accelerometer(micropython, window):
    window.roll_slider.setValue(30)
    x, _, _ = read(micropython, "Artemis.imu.get_accel()")
    assert x == pytest.approx(0.5, abs=0.03)

    click(window, "Level")
    x, _, _ = read(micropython, "Artemis.imu.get_accel()")
    assert x == pytest.approx(0, abs=0.02)
    assert window.roll_slider.value() == 0


def test_battery_slider_updates_adc(micropython, window):
    window.battery_slider.setValue(3200)
    micropython.run("adc = ADC(Pin(5), atten=ADC.ATTN_11DB)")
    assert read(micropython, "adc.read_uv()") == pytest.approx((3200 - 75) / 4 * 1000, rel=0.03)


def test_usb_and_charging_controls(micropython, window):
    micropython.run("usb = Pin(42, Pin.IN)\ncharge = Pin(7, Pin.IN)")
    assert read(micropython, "usb.value()") == 0
    assert read(micropython, "charge.value()") == 1

    click(window, "USB cable")
    assert read(micropython, "usb.value()") == 1

    click(window, "Charging")
    assert read(micropython, "charge.value()") == 0

    # Pulling the cable stops the charging too.
    click(window, "USB cable")
    assert read(micropython, "usb.value()") == 0
    assert read(micropython, "charge.value()") == 1
    window.refresh()
    assert not window.controls["Charging"].isChecked()


def test_tap_and_step_controls_preserve_session(micropython, window):
    click(window, "Tap")
    click(window, "Double tap")
    click(window, "+10 steps")
    QTest.keyClick(window, Qt.Key.Key_T)
    assert read(micropython, "1 + 1") == 2


def test_reset_key_and_button_restart_the_watch(micropython, window):
    watch = micropython.watch
    for act in (lambda: QTest.keyClick(window, Qt.Key.Key_R), lambda: click(window, "Reset")):
        boots = watch.usb_output.count("ESP-ROM:esp32s3")
        act()
        watch.wait_until(lambda: watch.usb_output.count("ESP-ROM:esp32s3") > boots, timeout=5)


def test_mute_key_and_checkbox(window):
    assert window.buzzer.muted
    QTest.keyClick(window, Qt.Key.Key_M)
    assert not window.buzzer.muted
    click(window, "Mute")
    assert window.buzzer.muted


def pump_for(buzzer, seconds):
    """Refreshes the buzzer as the window does, and returns the longest that took."""
    longest, end = 0.0, time.monotonic() + seconds
    while time.monotonic() < end:
        before = time.monotonic()
        buzzer.pump()
        longest = max(longest, time.monotonic() - before)
        time.sleep(0.01)
    return longest


def test_audio_probe_timeout_is_nonblocking(capfd):
    buzzer = Buzzer()
    buzzer.PROBE = "import time\ntime.sleep(60)\n"
    buzzer.PROBE_SECONDS = 0.5
    assert pump_for(buzzer, 1.5) < 0.25
    assert not buzzer.available
    assert buzzer._probe is None
    assert "No sound" in capfd.readouterr().err


def test_missing_audio_device_is_silent(capfd):
    buzzer = Buzzer()
    buzzer.PROBE = "import sys\nsys.exit(1)\n"
    assert pump_for(buzzer, 1.0) < 0.25
    assert not buzzer.available
    assert capfd.readouterr().err == ""


def test_disabled_buzzer_skips_audio_probe():
    buzzer = Buzzer(enabled=False)
    pump_for(buzzer, 0.1)
    assert buzzer._probe is None and not buzzer.available


def test_screenshot_key_and_button_save_the_display(micropython, window, tmp_path):
    window.screenshot_dir = str(tmp_path)
    micropython.watch.frame()
    QTest.keyClick(window, Qt.Key.Key_F12)
    shots = list(tmp_path.glob("artemis-*.png"))
    assert len(shots) == 1
    assert shots[0].read_bytes()[:4] == b"\x89PNG"


def shown(window):
    """The colour in the middle of the display, as the window draws it."""
    window.watch.frame()
    window.refresh()
    color = window.display.grab().toImage().pixelColor(window.display.lcd.center())
    return color.red(), color.green(), color.blue()


def test_display_brightness_tracks_backlight(micropython, window):
    micropython.run("d = Artemis.display\nd.fill(d.Color.Red)\nd.commit()")
    red, green, blue = shown(window)
    assert red > 200 and green < 40 and blue < 40

    micropython.run("Artemis.backlight.off()")
    assert shown(window)[0] < 60


def test_window_shows_the_leds(micropython, window):
    micropython.run("Artemis.leds[2].on()")
    window.refresh()
    lit = window.lamps[2].shown()
    assert (lit.red(), lit.green(), lit.blue()) == (255, 150, 30)
    assert window.lamps[0].shown() != lit


def test_window_stop_callback(window):
    frames = []

    def stop():
        frames.append(1)
        return len(frames) > 5

    window.run(stop)
    assert len(frames) > 5

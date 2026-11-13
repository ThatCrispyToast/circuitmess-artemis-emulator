"""The window's controls, with SDL's offscreen drivers standing in for a desktop."""

import os

os.environ["SDL_VIDEODRIVER"] = "dummy"
os.environ["SDL_AUDIODRIVER"] = "dummy"

import pygame
import pytest

from artemis_emu.gui import WatchWindow

SETUP = "import Artemis\nfrom machine import ADC, Pin\nArtemis.begin()\n"


@pytest.fixture
def window(micropython):
    micropython.run(SETUP)
    yield WatchWindow(micropython.watch, sound=False)
    pygame.quit()


def read(session, expression):
    return eval(session.run(f"print(repr({expression}))").strip())


def pin(session, number):
    return read(session, f"Pin({number}).value()")


def key(window, code, down=True):
    window.handle_event(pygame.event.Event(pygame.KEYDOWN if down else pygame.KEYUP, key=code))


def click(window, pos, button=1):
    window.handle_event(pygame.event.Event(pygame.MOUSEBUTTONDOWN, pos=pos, button=button))


def release(window, pos=(0, 0)):
    window.handle_event(pygame.event.Event(pygame.MOUSEBUTTONUP, pos=pos, button=1))


def control(window, label):
    return next(b for b in window.buttons if b.label == label).rect.center


@pytest.mark.parametrize(
    "code, gpio",
    [
        (pygame.K_LEFT, 2),
        (pygame.K_RIGHT, 1),
        (pygame.K_RETURN, 3),
        (pygame.K_SPACE, 3),
        (pygame.K_ESCAPE, 21),
        (pygame.K_BACKSPACE, 21),
    ],
)
def test_keys_work_the_lever_and_button(micropython, window, code, gpio):
    key(window, code)
    assert pin(micropython, gpio) == 1
    key(window, code, down=False)
    assert pin(micropython, gpio) == 0


@pytest.mark.parametrize(
    "zone, gpio",
    [("lever_left", 2), ("lever_right", 1), ("lever_push", 3), ("side_button", 21)],
)
def test_mouse_works_the_lever_and_button(micropython, window, zone, gpio):
    click(window, getattr(window, zone).center)
    assert pin(micropython, gpio) == 1
    release(window)
    assert pin(micropython, gpio) == 0


def test_tilt_keys_reach_the_accelerometer(micropython, window):
    key(window, pygame.K_w)
    key(window, pygame.K_w)
    key(window, pygame.K_w)
    _, y, _ = read(micropython, "Artemis.imu.get_accel()")
    assert y == pytest.approx(0.5, abs=0.02)

    key(window, pygame.K_0)
    x, y, z = read(micropython, "Artemis.imu.get_accel()")
    assert (x, y, z) == (
        pytest.approx(0, abs=0.02),
        pytest.approx(0, abs=0.02),
        pytest.approx(1, abs=0.02),
    )


def test_tilt_pad_reaches_the_accelerometer(micropython, window):
    pad = window.tilt_pad
    click(window, (pad.centerx + pad.width // 6, pad.centery))
    release(window)
    x, _, _ = read(micropython, "Artemis.imu.get_accel()")
    assert x == pytest.approx(0.5, abs=0.03)

    click(window, pad.center, button=3)
    x, _, _ = read(micropython, "Artemis.imu.get_accel()")
    assert x == pytest.approx(0, abs=0.02)


def test_battery_slider_reaches_the_adc(micropython, window):
    slider = window.battery_slider
    click(window, (slider.left, slider.centery))
    release(window)
    micropython.run("adc = ADC(Pin(5), atten=ADC.ATTN_11DB)")
    assert read(micropython, "adc.read_uv()") == pytest.approx((3200 - 75) / 4 * 1000, rel=0.03)


def test_usb_and_charging_controls(micropython, window):
    micropython.run("usb = Pin(42, Pin.IN)\ncharge = Pin(7, Pin.IN)")
    assert read(micropython, "usb.value()") == 0
    assert read(micropython, "charge.value()") == 1

    click(window, control(window, "USB cable"))
    assert read(micropython, "usb.value()") == 1

    click(window, control(window, "Charging"))
    assert read(micropython, "charge.value()") == 0

    click(window, window.usb_port.center)
    assert read(micropython, "usb.value()") == 0
    assert read(micropython, "charge.value()") == 1


def test_tap_and_step_controls_do_not_disturb_the_program(micropython, window):
    click(window, control(window, "Tap"))
    click(window, control(window, "Double tap"))
    click(window, control(window, "+10 steps"))
    key(window, pygame.K_t)
    assert read(micropython, "1 + 1") == 2


def test_reset_key_and_button_restart_the_watch(micropython, window):
    watch = micropython.watch
    for act in (lambda: key(window, pygame.K_r), lambda: click(window, control(window, "Reset"))):
        boots = watch.usb_output.count("ESP-ROM:esp32s3")
        act()
        watch.wait_until(lambda: watch.usb_output.count("ESP-ROM:esp32s3") > boots, timeout=5)


def test_mute_key_and_button(window):
    assert window.buzzer.muted
    key(window, pygame.K_m)
    assert not window.buzzer.muted
    click(window, control(window, "Mute"))
    assert window.buzzer.muted


def test_screenshot_key_saves_the_display(micropython, window, tmp_path):
    window.screenshot_dir = str(tmp_path)
    micropython.watch.frame()
    key(window, pygame.K_F12)
    shots = list(tmp_path.glob("artemis-*.png"))
    assert len(shots) == 1
    assert shots[0].read_bytes()[:4] == b"\x89PNG"


def show(window):
    window.watch.frame()
    window.render()
    return window.screen.get_at(window.lcd_rect.center)[:3]


def test_window_shows_the_display_dimmed_by_the_backlight(micropython, window):
    micropython.run("d = Artemis.display\nd.fill(d.Color.Red)\nd.commit()")
    red, green, blue = show(window)
    assert red > 200 and green < 40 and blue < 40

    micropython.run("Artemis.backlight.off()")
    assert show(window)[0] < 60


def test_window_runs_until_told_to_stop(window):
    frames = []

    def stop():
        frames.append(1)
        return len(frames) > 5

    window.run(stop)
    assert len(frames) > 5

"""Runs the example programs the way a test of your own would."""

import datetime as dt
import os
import threading
import time
from pathlib import Path

import pytest

from artemis_emu import PythonSession, Watch

EXAMPLES = Path(__file__).parent.parent / "examples"
FIRMWARE = os.environ.get("ARTEMIS_MICROPYTHON_FIRMWARE")


@pytest.fixture
def watch():
    if not FIRMWARE:
        pytest.skip("ARTEMIS_MICROPYTHON_FIRMWARE isn't set")
    with Watch(FIRMWARE, time=dt.datetime(2026, 7, 20, 20, 17, 5)) as watch:
        yield watch


def start_example(watch, name):
    session = PythonSession(watch)
    session.start((EXAMPLES / name).read_text())
    return session


def lit(watch, color):
    return sum(1 for pixel in watch.pixels() if pixel == color)


def test_hello_counts_lever_pushes_on_the_leds(watch):
    session = start_example(watch, "hello.py")
    watch.wait_until(lambda: lit(watch, (255, 255, 255)) > 50, timeout=5)
    assert watch.backlight == 1.0
    assert watch.leds == [0.0] * 6

    for count in (1, 2):
        watch.press("select", 0.15)
        watch.wait_until(lambda: sum(watch.leds) == count, timeout=2)
        time.sleep(0.15)  # the program polls: a press has to end for the next to count

    watch.set_button("alt", True)
    assert session.wait(timeout=5).strip() == "pushed 2 times"


def test_level_bubble_follows_the_tilt(watch):
    start_example(watch, "level.py")

    def bubble_centre():
        pixels = watch.pixels()
        spots = [
            (i % 128, i // 128) for i, p in enumerate(pixels) if p in ((255, 255, 0), (0, 255, 0))
        ]
        if not spots:
            return None
        return (sum(s[0] for s in spots) // len(spots), sum(s[1] for s in spots) // len(spots))

    watch.wait_until(lambda: bubble_centre() is not None, timeout=5)
    assert abs(bubble_centre()[0] - 64) < 3 and abs(bubble_centre()[1] - 64) < 3
    watch.wait_until(lambda: watch.rgb_led[1] > 0.99, timeout=2)

    watch.set_motion(accel_g=(0.5, 0.0, 0.87))
    watch.wait_until(lambda: bubble_centre() and bubble_centre()[0] < 50, timeout=3)
    assert watch.rgb_led[1] < 0.01


def test_clock_shows_the_rtc_on_the_leds(watch):
    start_example(watch, "clock.py")
    watch.wait_until(lambda: lit(watch, (255, 255, 255)) > 30, timeout=5)

    def seconds_from_leds():
        return sum(1 << i for i, level in enumerate(watch.leds) if level)

    first = seconds_from_leds()
    watch.wait_until(lambda: seconds_from_leds() != first, timeout=3)
    assert 5 <= seconds_from_leds() < 30


def test_scale_plays_every_note_in_order(watch):
    session = PythonSession(watch)
    heard = []
    runner = threading.Thread(target=session.run, args=((EXAMPLES / "scale.py").read_text(),))
    runner.start()
    while runner.is_alive():
        freq, _ = watch.buzzer
        if freq and (not heard or heard[-1] != freq):
            heard.append(freq)
    runner.join()
    assert [round(f / 10) for f in heard] == [52, 59, 66, 70, 78, 88]

"""Runs CircuitMess's unmodified factory firmware and checks the hardware it sees."""

import datetime as dt
import os
import time

from artemis_emu import Watch

ORANGE = (255, 101, 0)


def orange_pixels(watch):
    return sum(1 for r, g, b in watch.pixels() if r > 200 and 60 < g < 160 and b < 60)


def open_menu(watch):
    watch.press("select", 1.5)
    time.sleep(2)


def test_boots_to_the_lock_screen(booted):
    assert booted.powered
    assert booted.backlight > 0.9
    assert 1000 < orange_pixels(booted) < 16384


def test_console_reports_startup_on_usb_serial(stock):
    stock.wait_for_output("Hello", timeout=10)
    assert "ESP-ROM:esp32s3" in stock.usb_output
    # The battery driver reads its 2.5 V reference through the ADC at boot.
    match = stock.wait_for_output(r"Calibration: Read ([\d.]+) mV", timeout=10)
    assert abs(float(match.group(1)) - 2500) < 25


def test_all_hardware_probes_succeed(booted):
    for failure in ("IMU: Init error", "SPIFFS: mount failed", "Guru Meditation", "assert", "rtc_clk:"):
        assert failure not in booted.usb_output


def test_clock_shows_the_rtc(booted):
    before = booted.frame()
    booted.set_time(dt.datetime(2031, 12, 25, 7, 41))
    booted.wait_until(lambda: booted.frame() != before, timeout=5)


def test_buzzer_and_leds_play_the_startup_sequence():
    with Watch(os.environ["ARTEMIS_STOCK_FIRMWARE"]) as watch:
        seen_tone = set()
        seen_led = [False] * 6
        deadline = time.monotonic() + 11
        while time.monotonic() < deadline:
            freq, _ = watch.buzzer
            if freq:
                seen_tone.add(freq)
            for i, level in enumerate(watch.leds):
                seen_led[i] = seen_led[i] or level > 0.5
            time.sleep(0.005)

        assert {250, 500} <= seen_tone
        assert all(seen_led)
        # Timers driving the blinks have to keep firing: everything ends up off.
        watch.wait_until(lambda: not any(watch.leds), timeout=3)
        assert watch.buzzer == (0, 0.0)


def test_lever_and_button_navigate_the_menus(booted):
    lock_screen = booted.frame()
    open_menu(booted)
    menu = booted.frame()
    assert menu != lock_screen

    booted.press("down", 0.3)
    time.sleep(1.2)
    next_item = booted.frame()
    assert next_item != menu

    booted.press("select", 0.3)
    time.sleep(1.5)
    app = booted.frame()
    assert app != next_item

    booted.press("back", 0.3)
    time.sleep(1.5)
    assert booted.frame() != app


def test_tilting_moves_the_spirit_level(booted):
    open_menu(booted)
    booted.press("down", 0.3)
    time.sleep(1.2)
    booted.press("select", 0.3)
    time.sleep(1.5)

    level = booted.wait_until_display_settles()
    # This app doesn't sleep on tilt, so its bubbles should simply move.
    booted.tilt(pitch=20, roll=-25)
    tilted = booted.wait_until_display_settles()
    assert tilted != level

    booted.tilt()
    assert booted.wait_until_display_settles() != tilted


def test_button_sleeps_and_wakes_the_watch(booted):
    booted.press("back", 0.2)
    booted.wait_until(lambda: booted.backlight < 0.05, timeout=5)

    time.sleep(2)
    assert booted.backlight < 0.05

    booted.press("back", 0.3)
    booted.wait_until(lambda: booted.backlight > 0.9, timeout=5)


def test_lifting_the_wrist_wakes_the_watch(booted):
    booted.press("back", 0.2)
    booted.wait_until(lambda: booted.backlight < 0.05, timeout=5)
    time.sleep(1)

    # Y swings positive as the wrist comes up, then returns to rest.
    booted.set_motion((0.0, 0.8, 0.6))
    time.sleep(0.5)
    booted.set_motion((0.0, -0.8, 0.6))
    booted.wait_until(lambda: booted.backlight > 0.9, timeout=5)


def test_charging_lights_the_status_led(booted):
    booted.set_battery(usb=True, charging=True)
    booted.wait_until(lambda: booted.rgb_led[0] > 0.1, timeout=8)

    booted.set_battery(usb=False, charging=False)
    booted.wait_until(lambda: max(booted.rgb_led) < 0.02, timeout=8)


def test_battery_gauge_updates(booted):
    full = booted.frame()
    booted.set_battery(mv=3620)
    # The on-screen gauge changes once the filtered reading crosses a threshold.
    booted.wait_until(lambda: booted.frame() != full, timeout=40)


def test_low_battery_shutdown_and_button_restart(booted):
    booted.set_battery(mv=3200)
    booted.wait_until(lambda: not booted.powered, timeout=90)

    booted.set_battery(mv=3900)
    booted.press("back", 0.3)
    booted.wait_until(lambda: booted.powered, timeout=5)
    booted.wait_until(lambda: booted.usb_output.count("Hello") >= 2, timeout=15)


def test_flash_contents_persist_between_runs(tmp_path):
    flash = tmp_path / "flash.bin"
    firmware = os.environ["ARTEMIS_STOCK_FIRMWARE"]

    with Watch(firmware, flash_file=flash) as watch:
        # First boot has no PHY calibration saved, so it stores one in NVS.
        watch.wait_for_output("falling back to full calibration", timeout=15)
        time.sleep(3)

    with Watch(firmware, flash_file=flash) as watch:
        watch.wait_for_output("Hello", timeout=15)
        time.sleep(6)
        assert "falling back to full calibration" not in watch.usb_output

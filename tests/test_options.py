"""Choices the other tests leave at their defaults: releases, revisions and the emulator's switches."""

import datetime as dt
import os
import subprocess
import sys

import pytest

from artemis_emu import STOCK_VERSIONS, PythonSession, Watch, stock_firmware

MICROPYTHON = os.environ.get("ARTEMIS_MICROPYTHON_FIRMWARE")
STOCK_RELEASES = os.environ.get("ARTEMIS_STOCK_FIRMWARE_DIR")

needs_micropython = pytest.mark.skipif(not MICROPYTHON, reason="ARTEMIS_MICROPYTHON_FIRMWARE isn't set")
needs_stock = pytest.mark.skipif(not STOCK_RELEASES, reason="ARTEMIS_STOCK_FIRMWARE_DIR isn't set")


def emu(*args, timeout=60):
    return subprocess.run([sys.executable, "-m", "artemis_emu", *args],
                          capture_output=True, text=True, timeout=timeout)


def script(tmp_path, code):
    path = tmp_path / "script.py"
    path.write_text(code)
    return str(path)


# ---------------------------------------------------------------------------------------
# Stock firmware releases and hardware revisions

# Revision 2 runs every release; revision 3, with its newer display, came with 2.1.
@needs_stock
@pytest.mark.parametrize("version, revision",
                         [(version, 2) for version in STOCK_VERSIONS] +
                         [(version, 3) for version in STOCK_VERSIONS if version != "2.0"])
def test_stock_release_hardware_compatibility(version, revision):
    with Watch(stock_firmware(version), revision=revision) as watch:
        watch.wait_for_output("Hello", timeout=15)
        watch.wait_until(lambda: watch.backlight > 0.9, timeout=15)


@needs_stock
def test_stock_2_0_rejects_revision_3():
    with Watch(stock_firmware("2.0"), revision=3) as watch:
        watch.wait_for_output("Hardware check", timeout=15)


@needs_stock
def test_stock_cli_version_selection():
    result = emu("stock", "--version", "2.0", "--revision", "2", "--headless", "--mute", "--timeout", "4")
    assert result.returncode == 0, result.stderr
    assert "ESP-ROM:esp32s3" in result.stdout


def test_stock_cli_lists_supported_versions():
    result = emu("stock", "--version", "1.0", "--headless")
    assert result.returncode != 0
    assert "2.2" in result.stderr


# ---------------------------------------------------------------------------------------
# Switches

LOOP = """
import time
def loop():
    t0 = time.ticks_us()
    x = 0
    for i in range(30000):
        x += i
    return time.ticks_diff(time.ticks_us(), t0)
print(loop())
"""


@needs_micropython
def test_turbo_execution_speed():
    with Watch(MICROPYTHON, turbo=True) as watch:
        took_us = int(PythonSession(watch).run(LOOP))
    assert took_us < 60000          # 106 ms on a watch, and without turbo


@needs_micropython
def test_turbo_cli_option(tmp_path):
    result = emu("python", script(tmp_path, LOOP), "--turbo", "--headless", "--mute")
    assert result.returncode == 0, result.stderr
    assert int(result.stdout) < 60000


CLOCK_FLAG = """
from machine import I2C, Pin
i2c = I2C(0, sda=Pin(40), scl=Pin(41), freq=400000)
print(i2c.readfrom_mem(0x51, 2, 1)[0] >> 7)
"""


@needs_micropython
@pytest.mark.parametrize("time, lost_power", [("unset", 1), (None, 0),
                                              (dt.datetime(2026, 7, 20, 20, 17), 0)])
def test_rtc_initialization_flag(time, lost_power):
    with Watch(MICROPYTHON, time=time) as watch:
        assert int(PythonSession(watch).run(CLOCK_FLAG)) == lost_power


@needs_micropython
def test_unset_rtc_cli_option(tmp_path):
    result = emu("python", script(tmp_path, CLOCK_FLAG), "--time", "unset", "--headless", "--mute")
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "1"


ACCELEROMETER_READINGS = """
import time
from machine import I2C, Pin
i2c = I2C(0, sda=Pin(40), scl=Pin(41), freq=400000)
i2c.writeto_mem(0x6A, 0x10, b"\\x40")           # 104 Hz
time.sleep_ms(50)
seen = set()
for _ in range(20):
    seen.add(bytes(i2c.readfrom_mem(0x6A, 0x28, 6)))
    time.sleep_ms(12)
print(len(seen))
"""


@needs_micropython
@pytest.mark.parametrize("noise", [False, True])
def test_sensor_noise_option(noise):
    with Watch(MICROPYTHON, sensor_noise=noise) as watch:
        watch.set_motion(accel_g=(0.0, 0.0, 1.0))
        different = int(PythonSession(watch).run(ACCELEROMETER_READINGS))
    # A watch lying still gives a different reading nearly every time.
    assert different > 10 if noise else different == 1


# ---------------------------------------------------------------------------------------
# A real watch that isn't there

@pytest.mark.parametrize("command", [("device", "info"), ("device", "monitor"), ("repl", "--device"),
                                     ("device", "backup", "nowhere.bin")])
def test_device_commands_report_missing_port(command):
    port = ["/dev/no-such-watch"] if command[0] == "repl" else ["--port", "/dev/no-such-watch"]
    result = emu(*command, *port, timeout=30)
    assert result.returncode == 1
    assert "/dev/no-such-watch doesn't exist" in result.stderr

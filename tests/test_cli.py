"""The artemis-emu command, run as a separate process the way people use it."""

import json
import os
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

from artemis_emu.flash import APP_OFFSET, FLASH_SIZE, FirmwareError, build_flash_image

EXAMPLES = Path(__file__).parent.parent / "examples"
STOCK = os.environ.get("ARTEMIS_STOCK_FIRMWARE")
HEADLESS = ["--headless", "--mute"]


def emu(*args, timeout=60, **kwargs):
    return subprocess.run(
        [sys.executable, "-m", "artemis_emu", *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        **kwargs,
    )


def is_png(path):
    return Path(path).read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"


def script(tmp_path, code, name="script.py"):
    path = tmp_path / name
    path.write_text(code)
    return str(path)


def test_python_prints_what_the_script_prints_and_nothing_else(tmp_path):
    result = emu("python", script(tmp_path, "print('from the watch')"), *HEADLESS)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "from the watch"


def test_python_failure_gives_a_traceback_and_an_exit_code(tmp_path):
    result = emu("python", script(tmp_path, "raise ValueError('nope')"), *HEADLESS)
    assert result.returncode == 1
    assert "ValueError: nope" in result.stderr


def test_python_reports_a_script_that_never_ends(tmp_path):
    result = emu("python", script(tmp_path, "while True:\n    pass"), *HEADLESS, "--timeout", "2")
    assert result.returncode == 1
    assert "still running" in result.stderr


def test_python_reports_a_script_that_restarts_the_watch(tmp_path):
    result = emu("python", script(tmp_path, "import machine\nmachine.reset()"), *HEADLESS)
    assert result.returncode == 1
    assert "--install" in result.stderr


def test_python_takes_a_screenshot(tmp_path):
    shot = tmp_path / "screen.png"
    result = emu(
        "python",
        script(tmp_path, "from Artemis import *\nbegin()\ndisplay.fill(display.Color.Red)\ndisplay.commit()"),
        *HEADLESS,
        "--screenshot",
        str(shot),
    )
    assert result.returncode == 0, result.stderr
    assert is_png(shot)


def test_install_keeps_running_across_runs_with_a_flash_file(tmp_path):
    flash = tmp_path / "flash.bin"
    result = emu(
        "python", script(tmp_path, "print('installed and running')"),
        "--install", "--flash", str(flash), *HEADLESS, "--timeout", "5",
    )
    assert result.returncode == 0, result.stderr
    assert "installed and running" in result.stdout

    # No script this time: only what's in flash can print that.
    result = emu("repl", "--flash", str(flash), *HEADLESS, "--timeout", "5", stdin=subprocess.DEVNULL)
    assert result.returncode == 0, result.stderr
    assert "installed and running" in result.stdout


def test_stock_runs_headless_and_takes_a_screenshot(tmp_path):
    shot = tmp_path / "stock.png"
    result = emu("stock", *HEADLESS, "--timeout", "4", "--screenshot", str(shot))
    assert result.returncode == 0, result.stderr
    assert is_png(shot)
    assert "ESP-ROM:esp32s3" in result.stdout


def test_repl_is_interactive():
    controller, terminal = pty.openpty()
    process = subprocess.Popen(
        [sys.executable, "-m", "artemis_emu", "repl", *HEADLESS],
        stdin=terminal,
        stdout=terminal,
        stderr=terminal,
        close_fds=True,
    )
    os.close(terminal)
    seen = b""

    def read_until(token, timeout=30):
        nonlocal seen
        deadline = time.monotonic() + timeout
        while token not in seen:
            assert time.monotonic() < deadline, f"{token!r} not in {seen[-300:]!r}"
            if select.select([controller], [], [], 0.2)[0]:
                seen += os.read(controller, 4096)

    try:
        read_until(b">>> ")
        os.write(controller, b"print(6 * 7)\r")
        read_until(b"42")
        os.write(controller, b"\x1d")  # Ctrl-]
        assert process.wait(timeout=15) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(controller)


def test_gdb_stub_holds_the_watch_until_told_to_continue():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    process = subprocess.Popen(
        [sys.executable, "-m", "artemis_emu", "stock", *HEADLESS, "--gdb", str(port), "--wait-gdb"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                gdb = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                assert time.monotonic() < deadline
                time.sleep(0.05)
        with gdb:
            gdb.sendall(b"+$?#3f")
            assert b"$T" in gdb.recv(256) or b"$T" in gdb.recv(256)
            assert not select.select([process.stdout], [], [], 1)[0], "ran before being told to"

            gdb.sendall(b"+$c#63")
            seen = b""
            deadline = time.monotonic() + 15
            while b"ESP-ROM:esp32s3" not in seen:
                assert time.monotonic() < deadline, seen[-200:]
                if select.select([process.stdout], [], [], 0.2)[0]:
                    seen += os.read(process.stdout.fileno(), 4096)
    finally:
        process.terminate()
        process.wait(timeout=10)


@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGHUP, signal.SIGKILL])
def test_emulator_does_not_outlive_the_command(tmp_path, signum):
    process = subprocess.Popen(
        [sys.executable, "-m", "artemis_emu", "repl", *HEADLESS],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert b"ESP-ROM" in process.stdout.read1() + process.stdout.read1()
    children = Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
    assert len(children) == 1
    emulator = Path(f"/proc/{children[0]}/cmdline")
    assert b"qemu-system-xtensa" in emulator.read_bytes()

    process.send_signal(signum)
    process.wait(timeout=10)
    deadline = time.monotonic() + 10
    while emulator.exists():
        assert time.monotonic() < deadline, "the emulator is still running"
        time.sleep(0.05)


def test_window_opens_and_closes_cleanly(tmp_path):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    shot = tmp_path / "window.png"
    result = emu("stock", "--timeout", "3", "--screenshot", str(shot), env=env)
    assert result.returncode == 0, result.stderr
    assert is_png(shot)


# ----------------------------------------------------------------------
# What `run` accepts


@pytest.fixture
def stock_image(tmp_path):
    if not STOCK:
        pytest.skip("ARTEMIS_STOCK_FIRMWARE isn't set")
    return Path(STOCK).read_bytes()


def app_size(image):
    """The stock image's application, by its ESP image header and segments."""
    segments = image[APP_OFFSET + 1]
    offset = APP_OFFSET + 24
    for _ in range(segments):
        _, size = struct.unpack_from("<II", image, offset)
        offset += 8 + size
    # Padding to 16 bytes, checksum, and the appended SHA-256.
    return ((offset - APP_OFFSET) | 15) + 1 + 32


def boots(firmware, tmp_path):
    shot = tmp_path / "shot.png"
    result = emu("run", str(firmware), *HEADLESS, "--timeout", "12", "--screenshot", str(shot))
    assert result.returncode == 0, result.stderr
    assert "Hello" in result.stdout, result.stdout[-500:]
    return result


def test_run_accepts_a_merged_image(stock_image, tmp_path):
    boots(STOCK, tmp_path)


def test_run_puts_a_bare_app_behind_the_stock_bootloader(stock_image, tmp_path):
    app = tmp_path / "app.bin"
    app.write_bytes(stock_image[APP_OFFSET : APP_OFFSET + app_size(stock_image)])
    boots(app, tmp_path)


def test_run_accepts_an_esp_idf_build_directory(stock_image, tmp_path):
    build = tmp_path / "project" / "build"
    build.mkdir(parents=True)
    (build / "bootloader.bin").write_bytes(stock_image[:0x8000])
    (build / "partition-table.bin").write_bytes(stock_image[0x8000:0x9000])
    (build / "app.bin").write_bytes(stock_image[APP_OFFSET : APP_OFFSET + app_size(stock_image)])
    (build / "flasher_args.json").write_text(
        json.dumps(
            {
                "flash_files": {
                    "0x0": "bootloader.bin",
                    "0x8000": "partition-table.bin",
                    "0x10000": "app.bin",
                }
            }
        )
    )
    boots(tmp_path / "project", tmp_path)


def test_flash_image_is_always_the_size_of_the_flash_chip(stock_image):
    assert len(build_flash_image(STOCK)) == FLASH_SIZE


@pytest.mark.parametrize(
    "content, complaint",
    [
        (b"", "empty"),
        (b"\x7fELF" + bytes(64), "ELF"),
        (b"not firmware", "isn't an ESP32 firmware image"),
    ],
)
def test_run_explains_what_it_cannot_use(tmp_path, content, complaint):
    bad = tmp_path / "bad.bin"
    bad.write_bytes(content)
    with pytest.raises(FirmwareError, match=complaint):
        build_flash_image(bad)
    result = emu("run", str(bad), *HEADLESS)
    assert result.returncode == 1
    assert complaint in result.stderr

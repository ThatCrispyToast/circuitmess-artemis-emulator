"""Runs the emulated watch and exposes it to Python."""

from __future__ import annotations

import datetime as dt
import json
import math
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable, Iterable, Optional, Pattern, Union

from .bridge import Bridge, BoardState
from .efuse import DEFAULT_REVISION, build_efuse
from .flash import FLASH_SIZE, build_flash_image
from .image import rgb565_to_rgb888, save_png

# GPIO numbers from the Artemis Watch 2.0 schematic.
BUTTONS = {
    "up": 1,
    "lever_right": 1,
    "down": 2,
    "lever_left": 2,
    "select": 3,
    "lever_push": 3,
    "back": 21,
    "alt": 21,
    "button": 21,
}
LED_PINS = (46, 45, 44, 43, 18, 17)  # LED1..LED6
RGB_PINS = (14, 12, 13)  # active low
BACKLIGHT_PIN = 33  # active low
BUZZER_PIN = 47

LCD_WIDTH = 128
LCD_HEIGHT = 128


class EmulatorError(Exception):
    pass


def find_qemu() -> tuple[str, str]:
    """Returns the QEMU binary and its firmware data directory."""
    binary = os.environ.get("ARTEMIS_QEMU") or shutil.which("qemu-system-xtensa")
    if not binary:
        raise EmulatorError(
            "qemu-system-xtensa wasn't found: run through `nix run`, or set ARTEMIS_QEMU"
        )
    return binary, str(Path(binary).resolve().parent.parent / "share" / "qemu")


def _runtime_dir() -> str:
    # Unix socket paths are limited to about 100 bytes, so TMPDIR can't be trusted.
    for parent in (os.environ.get("XDG_RUNTIME_DIR"), "/tmp"):
        if parent and os.path.isdir(parent) and os.access(parent, os.W_OK):
            return tempfile.mkdtemp(prefix="artemis-", dir=parent)
    return tempfile.mkdtemp(prefix="artemis-")


def _connect_unix(path: str, timeout: float = 15.0) -> socket.socket:
    """Connects to a socket that QEMU may not have created yet."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.monotonic() + timeout
    while True:
        try:
            sock.connect(path)
            return sock
        except OSError:
            if time.monotonic() > deadline:
                sock.close()
                raise
            time.sleep(0.01)


class _SerialPort:
    """One end of a UART-like chardev; keeps everything it has ever received."""

    def __init__(self, path: str, on_data: Optional[Callable[[bytes], None]] = None):
        self.buffer = bytearray()
        self._cond = threading.Condition()
        self._on_data = on_data
        self._closed = False
        self._sock = _connect_unix(path)
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        while True:
            try:
                data = self._sock.recv(65536)
            except OSError:
                data = b""
            if not data:
                break
            with self._cond:
                self.buffer.extend(data)
                self._cond.notify_all()
            if self._on_data:
                self._on_data(data)
        with self._cond:
            self._closed = True
            self._cond.notify_all()

    def write(self, data: bytes) -> None:
        self._sock.sendall(data)

    def wait_for(self, predicate: Callable[[bytearray], bool], timeout: float) -> bool:
        with self._cond:
            return self._cond.wait_for(lambda: predicate(self.buffer) or self._closed, timeout)

    def close(self) -> None:
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()


class _QMP:
    def __init__(self, path: str):
        self._sock = _connect_unix(path)
        self._file = self._sock.makefile("r")
        self._file.readline()  # greeting
        self.command("qmp_capabilities")

    def command(self, name: str, **arguments) -> dict:
        msg: dict = {"execute": name}
        if arguments:
            msg["arguments"] = arguments
        self._sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            reply = json.loads(self._file.readline())
            if "return" in reply or "error" in reply:
                return reply

    def close(self) -> None:
        self._file.close()
        self._sock.close()


class Watch:
    """
    An emulated Artemis Watch 2.0, as a context manager:

        with Watch("firmware.bin") as watch:
            watch.wait_for_output("Hello")
            watch.press("select", 1.5)
            watch.screenshot("menu.png")
    """

    def __init__(
        self,
        firmware: Union[str, Path],
        *,
        revision: int = DEFAULT_REVISION,
        battery_mv: int = 3900,
        usb: bool = False,
        charging: bool = False,
        time: Optional[Union[dt.datetime, int]] = None,
        flash_file: Optional[Union[str, Path]] = None,
        psram_mb: int = 2,
        console: Optional[Callable[[bytes], None]] = None,
        gdb_port: Optional[int] = None,
        wait_for_gdb: bool = False,
        watchdogs: bool = True,
        qemu_args: Iterable[str] = (),
    ):
        self.firmware = Path(firmware)
        self.revision = revision
        self.battery_mv = battery_mv
        self.usb = usb
        self.charging = charging
        self.start_time = time
        self.flash_file = Path(flash_file) if flash_file else None
        self.psram_mb = psram_mb
        self.gdb_port = gdb_port
        self.wait_for_gdb = wait_for_gdb
        self.watchdogs = watchdogs
        self.qemu_args = list(qemu_args)
        self._console = console

        self.bridge: Optional[Bridge] = None
        self.usb_port: Optional[_SerialPort] = None
        self.uart_port: Optional[_SerialPort] = None
        self._qmp: Optional[_QMP] = None
        self._process: Optional[subprocess.Popen] = None
        self._run_dir: Optional[str] = None
        self._pending_uart: list[tuple[float, bytes]] = []
        self._recent_usb: list[bytes] = []
        self._uart_partial = b""
        self._usb_partial = b""
        self._console_lock = threading.Lock()
        self._console_timer: Optional[threading.Thread] = None
        self._stderr = None
        self._exit_status: Optional[int] = None

    # ------------------------------------------------------------------
    # Lifecycle

    def __enter__(self) -> "Watch":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def start(self) -> "Watch":
        qemu, data_dir = find_qemu()
        base = os.environ.get("ARTEMIS_STOCK_FIRMWARE")

        # Checks the firmware before anything is created that would need cleaning up.
        image = None
        if not self.flash_file or not self._flash_file_usable():
            image = build_flash_image(self.firmware, base)

        self._run_dir = _runtime_dir()
        run = Path(self._run_dir)

        if self.flash_file:
            flash_path = self.flash_file
            if image:
                flash_path.parent.mkdir(parents=True, exist_ok=True)
                flash_path.write_bytes(image)
        else:
            flash_path = run / "flash.bin"
            flash_path.write_bytes(image)

        efuse_path = run / "efuse.bin"
        efuse_path.write_bytes(build_efuse(self.revision))

        bridge_sock = str(run / "bridge")
        usb_sock = str(run / "usb")
        uart_sock = str(run / "uart")
        qmp_sock = str(run / "qmp")

        args = [
            qemu,
            "-L", data_dir,
            "-M", "esp32s3",
            "-m", f"{self.psram_mb}M",
            "-display", "none",
            "-monitor", "none",
            "-S",
            "-qmp", f"unix:{qmp_sock},server=on,wait=off",
            "-drive", f"file={flash_path},if=mtd,format=raw",
            "-drive", f"file={efuse_path},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
            "-chardev", f"socket,id=artemis-bridge,path={bridge_sock},server=on,wait=off",
            "-chardev", f"socket,id=artemis-usb,path={usb_sock},server=on,wait=off",
            "-chardev", f"socket,id=uart0,path={uart_sock},server=on,wait=off",
            "-serial", "chardev:uart0",
            # The long form, because the device's own name contains a dot.
            "-global", f"driver=artemis.board,property=revision,value={self.revision}",
            "-global", f"driver=artemis.board,property=battery-mv,value={self.battery_mv}",
            "-global", f"driver=artemis.board,property=usb,value={'on' if self.usb else 'off'}",
            "-global", f"driver=artemis.board,property=charging,value={'on' if self.charging else 'off'}",
        ]
        if not self.watchdogs:
            args += ["-global", "driver=timer.esp.timg,property=wdt_disable,value=on"]
        if self.gdb_port:
            args += ["-gdb", f"tcp::{self.gdb_port}"]
        args += self.qemu_args

        self._stderr = open(run / "qemu.stderr", "w+b")
        self._process = subprocess.Popen(args, stderr=self._stderr)

        try:
            deadline = time.monotonic() + 15
            while not os.path.exists(qmp_sock):
                if self._process.poll() is not None:
                    raise EmulatorError(f"QEMU exited: {self.emulator_log()}")
                if time.monotonic() > deadline:
                    raise EmulatorError("QEMU didn't start")
                time.sleep(0.01)

            self._qmp = _QMP(qmp_sock)
            self.bridge = Bridge(bridge_sock)
            self.usb_port = _SerialPort(usb_sock, self._usb_data)
            self.uart_port = _SerialPort(uart_sock, self._uart_data)

            # QEMU has everything open, so nothing can be left behind however this run ends.
            self._remove_run_dir()

            if self.start_time is not None:
                self.set_time(self.start_time)

            if not self.wait_for_gdb:
                self.resume()
        except BaseException:
            self.stop()
            raise

        return self

    def _flash_file_usable(self) -> bool:
        return self.flash_file.exists() and self.flash_file.stat().st_size == FLASH_SIZE

    def _remove_run_dir(self) -> None:
        if self._run_dir:
            shutil.rmtree(self._run_dir, ignore_errors=True)
            self._run_dir = None

    def stop(self) -> None:
        for closer in (self.bridge, self.usb_port, self.uart_port, self._qmp):
            if closer:
                try:
                    closer.close()
                except OSError:
                    pass
        self.bridge = self.usb_port = self.uart_port = self._qmp = None

        if self._process:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait()
            self._exit_status = self._process.returncode
            self._process = None

        self._remove_run_dir()

    @property
    def running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    def emulator_log(self) -> str:
        """What QEMU itself has reported, such as why it stopped."""
        if not self._stderr:
            return ""
        log = os.pread(self._stderr.fileno(), 1 << 20, 0).decode(errors="replace").strip()
        status = self._process.poll() if self._process else self._exit_status
        if status is not None and status < 0 and status != -signal.SIGTERM:
            log += f"\nQEMU died from signal {-status}"
        return log

    def pause(self) -> None:
        self._qmp.command("stop")

    def resume(self) -> None:
        self._qmp.command("cont")

    def reset(self) -> None:
        """Cuts power and boots again, as holding the chip in reset would."""
        self.bridge.reset()

    # ------------------------------------------------------------------
    # Inputs

    def set_button(self, button: str, pressed: bool) -> None:
        self.bridge.set_pin(self._button_pin(button), 1 if pressed else 0)
        self.bridge.sync()

    def press(self, button: str, duration: float = 0.15) -> None:
        self.set_button(button, True)
        time.sleep(duration)
        self.set_button(button, False)

    @staticmethod
    def _button_pin(button: str) -> int:
        try:
            return BUTTONS[button]
        except KeyError:
            raise ValueError(f"unknown button {button!r}: use one of {sorted(BUTTONS)}") from None

    def set_motion(
        self,
        accel_g: tuple[float, float, float] = (0.0, 0.0, 1.0),
        gyro_dps: tuple[float, float, float] = (0.0, 0.0, 0.0),
        temperature: float = 25.0,
    ) -> None:
        """Raw IMU axes: acceleration in g, rotation in degrees per second."""
        self.bridge.set_imu(
            tuple(a * 1000 for a in accel_g),
            tuple(g * 1000 for g in gyro_dps),
            temperature,
        )
        self.bridge.sync()

    def tilt(self, pitch: float = 0.0, roll: float = 0.0) -> None:
        """Holds the watch still at an angle, in degrees. Level and face up is 0, 0."""
        p, r = math.radians(pitch), math.radians(roll)
        self.set_motion((math.sin(r) * math.cos(p), math.sin(p), math.cos(r) * math.cos(p)))

    def tap(self, axis: str = "z", double: bool = False) -> None:
        self.bridge.tap("xyz".index(axis), double)
        self.bridge.sync()

    def steps(self, count: int) -> None:
        self.bridge.steps(count)
        self.bridge.sync()

    def set_battery(
        self,
        mv: Optional[int] = None,
        usb: Optional[bool] = None,
        charging: Optional[bool] = None,
    ) -> None:
        if mv is not None:
            self.battery_mv = mv
        if usb is not None:
            self.usb = usb
        if charging is not None:
            self.charging = charging
        self.bridge.set_battery(self.battery_mv, self.usb, self.charging)
        self.bridge.sync()

    def set_time(self, when: Union[dt.datetime, int]) -> None:
        """Sets the RTC. The firmware shows it as-is, so a datetime means wall-clock time."""
        if isinstance(when, dt.datetime):
            epoch = int(when.replace(tzinfo=dt.timezone.utc).timestamp())
        else:
            epoch = when
        self.bridge.set_rtc(epoch)
        self.bridge.sync()

    # ------------------------------------------------------------------
    # Outputs. Each reads the hardware as it is right now, as of the call.

    @property
    def state(self) -> BoardState:
        """The last state reported, without waiting for the board to catch up."""
        return self.bridge.state

    @property
    def powered(self) -> bool:
        self.bridge.sync()
        return self.state.powered

    def _pin_fraction(self, pin: int) -> float:
        st = self.state.pins.get(pin)
        return st.level / 65535 if st else 0.0

    @property
    def leds(self) -> list[float]:
        """Brightness of LED1..LED6, each from 0 to 1."""
        self.bridge.sync()
        return [self._pin_fraction(p) for p in LED_PINS]

    @property
    def rgb_led(self) -> tuple[float, float, float]:
        """Brightness of the RGB LED's channels, each from 0 to 1. The LED is common-anode."""
        self.bridge.sync()
        r, g, b = (1 - self._pin_fraction(p) for p in RGB_PINS)
        return (r, g, b)

    @property
    def backlight(self) -> float:
        """Backlight brightness from 0 to 1. Its driver is active low."""
        self.bridge.sync()
        if BACKLIGHT_PIN not in self.state.pins:
            return 0.0
        return 1 - self._pin_fraction(BACKLIGHT_PIN)

    @property
    def buzzer(self) -> tuple[int, float]:
        """Frequency in Hz and duty cycle. Silent means a frequency of 0."""
        self.bridge.sync()
        st = self.state.pins.get(BUZZER_PIN)
        if not st or not st.level or not st.freq:
            return (0, 0.0)
        return (st.freq, st.level / 65535)

    def frame(self, fresh: bool = True) -> bytes:
        """The display as little-endian RGB565, top-left first, 128x128."""
        if fresh:
            self.bridge.request_frame()
            self.bridge.next_frame()
        return self.state.frame

    def pixels(self, fresh: bool = True) -> list[tuple[int, int, int]]:
        rgb = rgb565_to_rgb888(self.frame(fresh))
        return [tuple(rgb[i : i + 3]) for i in range(0, len(rgb), 3)]

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        return self.pixels()[y * LCD_WIDTH + x]

    def screenshot(self, path: Union[str, Path], scale: int = 3) -> None:
        save_png(str(path), self.frame(), LCD_WIDTH, LCD_HEIGHT, scale)

    def wait_until(
        self, condition: Callable[[], bool], timeout: float = 10.0, interval: float = 0.02
    ) -> None:
        deadline = time.monotonic() + timeout
        while not condition():
            if not self.running:
                raise EmulatorError(f"the emulator exited while waiting: {self.emulator_log()}")
            if time.monotonic() > deadline:
                raise TimeoutError(f"condition not met after {timeout} s")
            time.sleep(interval)

    def wait_for_frame_change(self, timeout: float = 5.0) -> bytes:
        before = self.frame(fresh=False)
        self.wait_until(lambda: self.frame(fresh=False) != before, timeout)
        return self.frame(fresh=False)

    def wait_until_display_settles(self, quiet: float = 0.3, timeout: float = 10.0) -> bytes:
        """Waits for animations to finish: no pixel changes for `quiet` seconds."""
        deadline = time.monotonic() + timeout
        last = self.frame(fresh=False)
        since = time.monotonic()
        while time.monotonic() - since < quiet:
            if time.monotonic() > deadline:
                raise TimeoutError(f"display still changing after {timeout} s")
            time.sleep(0.02)
            now = self.frame(fresh=False)
            if now != last:
                last, since = now, time.monotonic()
        return last

    # ------------------------------------------------------------------
    # Serial consoles. The USB port is the one both the stock firmware and
    # MicroPython use; ESP-IDF's default configuration uses UART0.

    @property
    def usb_output(self) -> str:
        return self.usb_port.buffer.decode(errors="replace")

    @property
    def uart_output(self) -> str:
        return self.uart_port.buffer.decode(errors="replace")

    @property
    def output(self) -> str:
        return self.usb_output + self.uart_output

    def write_serial(self, text: Union[str, bytes]) -> None:
        data = text.encode() if isinstance(text, str) else text
        self.usb_port.write(data)
        self.uart_port.write(data)

    def wait_for_output(
        self, pattern: Union[str, Pattern[str]], timeout: float = 15.0
    ) -> re.Match:
        """Waits for a regular expression to match console output, from either port."""
        regex = re.compile(pattern) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        while True:
            match = regex.search(self.output)
            if match:
                return match
            if not self.running:
                raise EmulatorError(f"the emulator exited: {self.emulator_log()}")
            if time.monotonic() > deadline:
                raise TimeoutError(f"{regex.pattern!r} not seen after {timeout} s")
            time.sleep(0.02)

    # The ROM and bootloader print the same lines on both ports. Show UART lines
    # after a short delay, dropping any that the USB port has just shown.
    def _usb_data(self, data: bytes) -> None:
        if self._console:
            self._console(data)
        lines = (self._usb_partial + data).split(b"\n")
        self._usb_partial = lines.pop()
        with self._console_lock:
            self._recent_usb.extend(line.rstrip(b"\r") for line in lines)
            del self._recent_usb[:-64]

    def _uart_data(self, data: bytes) -> None:
        if not self._console:
            return
        lines = (self._uart_partial + data).split(b"\n")
        self._uart_partial = lines.pop()
        now = time.monotonic()
        with self._console_lock:
            self._pending_uart.extend((now, line.rstrip(b"\r")) for line in lines)
        if self._console_timer is None:
            self._console_timer = threading.Thread(target=self._flush_uart, daemon=True)
            self._console_timer.start()

    def _flush_uart(self) -> None:
        while self.running:
            time.sleep(0.05)
            now = time.monotonic()
            with self._console_lock:
                while self._pending_uart and now - self._pending_uart[0][0] > 0.05:
                    _, line = self._pending_uart.pop(0)
                    if line in self._recent_usb:
                        self._recent_usb.remove(line)
                    else:
                        self._console(line + b"\n")


def write_to_stdout(data: bytes) -> None:
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()

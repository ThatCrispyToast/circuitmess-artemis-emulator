"""A real Artemis Watch on USB: its serial console, and esptool for its flash."""

from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable, Optional, Union

from .firmware import describe, partitions
from .machine import EmulatorError
from .flash import APP_OFFSET, FLASH_SIZE

# The watch enumerates as the ESP32-S3's built-in USB Serial/JTAG bridge.
USB_VENDOR, USB_PRODUCT = "303a", "1001"

# MicroPython's filesystem partition in CircuitMess's build.
_VFS_OFFSET, _VFS_SIZE = 0x200000, 0x200000
# EFUSE_RD_USR_DATA0: product ID in the upper half, then the revision in the next word.
_EFUSE_USER_DATA = 0x6000707C


class DeviceError(Exception):
    pass


def find_ports() -> list[str]:
    """Serial ports of every ESP32 USB Serial/JTAG bridge that's plugged in."""
    ports = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        usb = os.path.realpath(os.path.join(tty, "device", ".."))
        try:
            ids = tuple(Path(usb, name).read_text().strip() for name in ("idVendor", "idProduct"))
        except OSError:
            continue
        if ids == (USB_VENDOR, USB_PRODUCT):
            ports.append("/dev/" + os.path.basename(tty))
    return ports


def _serial_number(port: str) -> Optional[str]:
    usb = os.path.realpath(os.path.join("/sys/class/tty", os.path.basename(port), "device", ".."))
    try:
        return Path(usb, "serial").read_text().strip()
    except OSError:
        return None


class _DevicePort:
    """The watch's serial port, with the interface of the emulator's serial ports."""

    def __init__(self, path: str, on_data: Optional[Callable[[bytes], None]] = None):
        import serial

        self.buffer = bytearray()
        self._cond = threading.Condition()
        self._on_data = on_data
        self.closed = False
        try:
            # DTR and RTS both asserted is the bridge's idle state: opening doesn't reset.
            self._serial = serial.Serial(path, 115200, timeout=0.05, write_timeout=2)
        except serial.SerialException as e:
            raise DeviceError(_explain_open_failure(path, e)) from None
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        while not self.closed:
            try:
                data = self._serial.read(4096)
            except Exception:
                # Unplugged, or the chip dropped off the bus by sleeping or resetting.
                break
            if data:
                with self._cond:
                    self.buffer.extend(data)
                    self._cond.notify_all()
                if self._on_data:
                    self._on_data(data)
        with self._cond:
            self.closed = True
            self._cond.notify_all()

    def write(self, data: bytes) -> None:
        try:
            self._serial.write(data)
            self._serial.flush()
        except Exception as e:
            raise OSError(str(e)) from None

    def wait_for(self, predicate: Callable[[bytearray], bool], timeout: float) -> bool:
        with self._cond:
            return self._cond.wait_for(lambda: predicate(self.buffer) or self.closed, timeout)

    def reset_chip(self) -> None:
        """Pulses the chip's reset through the bridge, booting the firmware in flash."""
        self._serial.dtr = False  # with RTS still asserted, this holds the chip in reset
        time.sleep(0.1)
        self._serial.rts = False
        time.sleep(0.05)
        self._serial.dtr = True
        self._serial.rts = True

    def close(self) -> None:
        self.closed = True
        try:
            self._serial.close()
        except Exception:
            pass
        self._thread.join(timeout=1)


def _explain_open_failure(path: str, error: Exception) -> str:
    if isinstance(getattr(error, "errno", None), int) and error.errno == 13 or "Permission" in str(error):
        return (
            f"no permission to open {path}. Give your user access to the watch's USB "
            "serial port (a udev rule tagging USB ID 303a:1001 with uaccess, or the dialout group)"
        )
    return f"can't open {path}: {error}"


class Device:
    """
    A watch plugged in over USB, as a context manager. It takes the place of the emulated
    Watch wherever only the serial console is needed:

        with Device() as watch:
            print(PythonSession(watch).run("print(1 + 1)"))
    """

    def __init__(
        self,
        port: Optional[str] = None,
        console: Optional[Callable[[bytes], None]] = None,
    ):
        self.port = port
        self._console = console
        self._serial_number: Optional[str] = None
        self._in_bootloader = False
        self.usb_port: Optional[_DevicePort] = None
        # When the firmware last started from scratch, on time.monotonic()'s clock.
        self.restarted_at = 0.0

    def __enter__(self) -> "Device":
        self.open()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ------------------------------------------------------------------
    # The serial console

    def _resolve_port(self) -> str:
        if self.port and os.path.exists(self.port):
            return self.port
        ports = find_ports()
        if self._serial_number:
            ports = [p for p in ports if _serial_number(p) == self._serial_number]
        if self.port and not self._serial_number:
            raise DeviceError(f"{self.port} doesn't exist")
        if not ports:
            raise DeviceError("no watch found: is it plugged in and switched on?")
        if len(ports) > 1 and not self._serial_number:
            raise DeviceError(f"several devices found ({', '.join(ports)}): choose one with a port")
        return ports[0]

    def open(self) -> "Device":
        self.port = self._resolve_port()
        self._serial_number = _serial_number(self.port)
        self.usb_port = _DevicePort(self.port, self._usb_data)
        return self

    def close(self) -> None:
        if self.usb_port:
            self.usb_port.close()
            self.usb_port = None

    def _usb_data(self, data: bytes) -> None:
        if self._console:
            self._console(data)

    @property
    def running(self) -> bool:
        return self.usb_port is not None and not self.usb_port.closed

    def emulator_log(self) -> str:
        return "the watch's serial port closed"

    @property
    def usb_output(self) -> str:
        return self.usb_port.buffer.decode(errors="replace")

    output = usb_output

    def write_serial(self, text: Union[str, bytes]) -> None:
        self.usb_port.write(text.encode() if isinstance(text, str) else text)

    def wait_for_output(self, pattern: str, timeout: float = 15.0) -> re.Match:
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            match = regex.search(self.usb_output)
            if match:
                return match
            if time.monotonic() > deadline:
                raise TimeoutError(f"{regex.pattern!r} not seen after {timeout} s")
            time.sleep(0.02)

    def reconnect(self, timeout: float = 15.0) -> None:
        """Reopens the port after the watch dropped off USB, as it does across deep sleep."""
        kept = bytes(self.usb_port.buffer) if self.usb_port else b""
        self.close()
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.port = None
                self.open()
                break
            except DeviceError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.usb_port.buffer[:0] = kept

    def reset(self) -> None:
        """Restarts the watch, as its reset line would."""
        self.usb_port.reset_chip()
        self.restarted_at = time.monotonic()

    def recover(self) -> None:
        """
        Restarts the watch by way of its bootloader. Bytes sent while no firmware was
        reading them stay in the chip's USB buffer through a plain reset, and MicroPython
        then never reads its input again; the bootloader empties that buffer.
        """
        self.esptool("chip-id")

    # ------------------------------------------------------------------
    # Flash, through esptool. The serial console is closed while it runs.

    def esptool(self, *args: str, quiet: bool = True, stay: bool = False) -> str:
        """
        Runs an esptool command against the watch. `stay` leaves the chip in its
        bootloader for the next command; otherwise it restarts into its firmware.
        """
        binary = shutil.which("esptool")
        if not binary:
            raise DeviceError("esptool wasn't found: run through `nix run` or `nix develop`")
        was_open = self.usb_port is not None
        self.port = self.port or self._resolve_port()
        self.close()

        # esptool's own reset at the end only works if it also did the one at the start,
        # so it's always told to leave the chip alone and the restart is done here.
        command = [binary, "--chip", "esp32s3", "--port", self.port,
                   "--before", "no-reset" if self._in_bootloader else "default-reset",
                   "--after", "no-reset"]
        try:
            result = subprocess.run(
                command + list(args),
                stdout=subprocess.PIPE if quiet else None,
                stderr=subprocess.STDOUT if quiet else None,
                text=True,
            )
            self._in_bootloader = True
            if not stay or result.returncode != 0:
                self._leave_bootloader()
        finally:
            if was_open:
                self.reconnect()
        if result.returncode != 0:
            raise DeviceError(f"esptool failed:\n{(result.stdout or '').strip()}")
        return result.stdout or ""

    def _leave_bootloader(self) -> None:
        port = _DevicePort(self.port)
        try:
            port.reset_chip()
        finally:
            port.close()
        self._in_bootloader = False
        self.restarted_at = time.monotonic()

    def _read_region(self, offset: int, size: int, stay: bool = True) -> bytes:
        with tempfile.TemporaryDirectory(prefix="artemis-") as scratch:
            path = os.path.join(scratch, "region.bin")
            self.esptool("read-flash", hex(offset), hex(size), path, stay=stay)
            return Path(path).read_bytes()

    def _read_word(self, address: int) -> int:
        out = self.esptool("read-mem", hex(address), stay=True)
        match = re.search(r"=\s*(0x[0-9a-fA-F]+)", out)
        if not match:
            raise DeviceError(f"couldn't read {address:#x}:\n{out.strip()}")
        return int(match.group(1), 16)

    def info(self) -> dict:
        """What the watch is: chip, flash, hardware revision and the firmware on it."""
        out = self.esptool("flash-id", stay=True)

        def field(name: str) -> str:
            match = re.search(rf"^{name}:\s*(.+)$", out, re.M)
            return match.group(1).strip() if match else "unknown"

        # The factory writes a product ID and hardware revision into the user eFuse block.
        product = self._read_word(_EFUSE_USER_DATA) >> 16
        revision = self._read_word(_EFUSE_USER_DATA + 4) & 0xFF
        head = self._read_region(0, APP_OFFSET + 0x200, stay=False)
        firmware = describe(head)
        if "stock firmware" not in firmware:
            # MicroPython builds don't name themselves in their image, but they answer.
            firmware = self._micropython_banner() or firmware
        return {
            "port": self.port,
            "chip": field("Chip type"),
            "features": field("Features"),
            "mac": field("MAC"),
            "flash": field("Detected flash size"),
            "product": product,
            "revision": revision,
            "firmware": firmware,
            "partitions": partitions(head),
        }

    def _micropython_banner(self) -> Optional[str]:
        """How MicroPython on the watch introduces itself, if that's what is running."""
        from .micropython import PythonSession

        was_open = self.usb_port is not None
        try:
            if not was_open:
                self.open()
            session = PythonSession(self, boot_timeout=4.0)
            name = session.run("import os\nu = os.uname()\nprint(u.version, '-', u.machine)", timeout=5)
            session.soft_reset()
            return "MicroPython " + name.strip()
        except (TimeoutError, DeviceError, EmulatorError, OSError):
            return None
        finally:
            if not was_open:
                self.close()

    def backup(self, path: Union[str, Path], quiet: bool = True) -> str:
        """
        Saves the whole flash to a file, reading it twice to be sure of it, and returns
        what firmware it holds. The watch restarts into its firmware afterwards.
        """
        path = Path(path)
        check = path.with_name(path.name + ".check")
        try:
            self.esptool("read-flash", "0", hex(FLASH_SIZE), str(path), quiet=quiet, stay=True)
            self.esptool("read-flash", "0", hex(FLASH_SIZE), str(check), quiet=quiet)
            image = path.read_bytes()
            if image != check.read_bytes():
                path.unlink()
                raise DeviceError("the flash read back differently twice: the backup can't be trusted")
        finally:
            check.unlink(missing_ok=True)
        return describe(image)

    def write_flash(self, image: Union[str, Path], offset: int = 0, quiet: bool = True) -> None:
        """Writes an image to flash, checks it, and restarts the watch into it."""
        self.esptool("write-flash", hex(offset), str(image), quiet=quiet)

    def install_micropython(self, image: Union[str, Path], keep_files: bool = True,
                            quiet: bool = True) -> bool:
        """
        Flashes CircuitMess's MicroPython. Where its filesystem goes, another firmware
        will have left data that MicroPython refuses to start on, so that region is
        erased unless it already holds a filesystem to keep. Returns whether it was.
        """
        start = self._read_region(_VFS_OFFSET, 4096)
        blank = start.count(0xFF) == len(start)
        has_files = start[510:512] == b"\x55\xaa" or start[8:16] == b"littlefs"
        erase = not blank and not (keep_files and has_files)
        if erase:
            self.esptool("erase-region", hex(_VFS_OFFSET), hex(_VFS_SIZE), quiet=quiet, stay=True)
        self.write_flash(image, quiet=quiet)
        return erase

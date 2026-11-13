"""Runs Python on a watch, emulated or real, over the serial REPL as mpremote does."""

from __future__ import annotations

import time
from typing import Callable, Optional

from .machine import EmulatorError

_RAW_PROMPT = b"raw REPL; CTRL-B to exit\r\n>"
_BOOT_BANNER = b"ESP-ROM:"
# What MicroPython prints once it's reading its input, and how long that takes at
# the outside when it prints nothing, as when main.py runs straight away.
_UP_SIGNS = (b"MicroPython v", b">>> ", b"Performing initial setup")
_BOOT_TIME = 1.0
_NEVER_ANSWERED = "MicroPython's REPL never answered: is this a MicroPython firmware?"
# Asks the raw REPL for raw-paste mode, where the watch paces what it's sent.
_RAW_PASTE = b"\x05A\x01"


class PythonError(Exception):
    """A script raised an exception on the watch."""

    def __init__(self, traceback: str):
        super().__init__(traceback)
        self.traceback = traceback


class WatchRestarted(Exception):
    """The watch rebooted before the script finished, taking the script with it."""


class PythonSession:
    """
    Talks to MicroPython's raw REPL on the watch's USB serial port. The watch is an
    emulated `Watch` or a real one, a `Device`.
    """

    def __init__(self, watch, boot_timeout: float = 60.0):
        self.watch = watch
        self.port = watch.usb_port
        self._mark = 0
        self._started = 0
        self._enter_raw(boot_timeout)

    def _wait(self, token: bytes, timeout: float) -> bytes:
        """Waits for a token after the read mark, and returns everything up to it."""
        def found(buf: bytearray) -> bool:
            return buf.find(token, self._mark) >= 0

        if not self.port.wait_for(found, timeout) or not found(self.port.buffer):
            tail = bytes(self.port.buffer[self._mark:])[-200:]
            raise TimeoutError(f"{token!r} not seen on the watch's serial port; last output: {tail!r}")
        end = self.port.buffer.find(token, self._mark)
        data = bytes(self.port.buffer[self._mark : end])
        self._mark = end + len(token)
        return data

    def _read(self, count: int, timeout: float) -> bytes:
        """Takes the next bytes after the read mark."""
        end = self._mark + count
        if not self.port.wait_for(lambda buf: len(buf) >= end, timeout) or len(self.port.buffer) < end:
            tail = bytes(self.port.buffer[self._mark:])[-200:]
            raise TimeoutError(f"the watch stopped answering; last output: {tail!r}")
        data = bytes(self.port.buffer[self._mark : end])
        self._mark = end
        return data

    def _write(self, data: bytes) -> None:
        try:
            self.port.write(data)
        except OSError:
            raise EmulatorError(f"the watch is gone: {self.watch.emulator_log()}") from None

    def _wait_until_listening(self, timeout: float) -> bool:
        """
        After a restart, holds off until MicroPython is up. On the watch, bytes that
        arrive before it starts reading its USB port are never read, and stop
        everything sent after them from getting through. False if it never got there.
        """
        if hasattr(type(self.watch), "usb_listening"):
            # The emulator can say; a real watch has to be judged by what it prints.
            deadline = time.monotonic() + timeout
            while self.watch.running and not self.watch.usb_listening:
                if time.monotonic() > deadline:
                    return False
                time.sleep(0.01)
            return True

        deadline = time.monotonic() + _BOOT_TIME
        while time.monotonic() < deadline:
            buffer = self.port.buffer
            boot = buffer.rfind(_BOOT_BANNER)
            recently = time.monotonic() - self.watch.restarted_at < _BOOT_TIME
            if boot < 0 and not recently:
                break  # it has been running all along
            if any(buffer.find(sign, max(boot, 0)) >= 0 for sign in _UP_SIGNS):
                break
            time.sleep(0.01)
        return True

    def _enter_raw(self, timeout: float) -> None:
        if not self._wait_until_listening(timeout):
            raise TimeoutError(_NEVER_ANSWERED)
        deadline = time.monotonic() + timeout
        while True:
            self._mark = len(self.port.buffer)
            # Ctrl-C twice stops a running main.py, Ctrl-A selects raw mode.
            self._write(b"\r\x03\x03")
            time.sleep(0.1)
            self._write(b"\x01")
            try:
                self._wait(_RAW_PROMPT, 1.0)
                return
            except TimeoutError:
                if time.monotonic() > deadline:
                    raise TimeoutError(_NEVER_ANSWERED) from None

    def start(self, code: str) -> None:
        """Starts code without waiting for it, to interact with it or if it restarts the watch."""
        data = code.encode()
        self._mark = len(self.port.buffer)
        self._write(_RAW_PASTE)
        try:
            answer = self._read(2, 2.0)
        except TimeoutError:
            answer = b""

        if answer == b"R\x01":
            self._paste(data)
        else:
            # A MicroPython without raw-paste mode: start over and send it all at once,
            # which is fine for as much as its input buffer holds.
            if answer != b"R\x00":
                self._enter_raw(5.0)
            self._mark = len(self.port.buffer)
            for i in range(0, len(data), 256):
                self._write(data[i : i + 256])
                time.sleep(0.001)
            self._write(b"\x04")
            self._wait(b"OK", 10.0)
        self._started = self._mark

    def _paste(self, data: bytes) -> None:
        """Sends code in raw-paste mode: no more at a time than the watch has asked for."""
        window = int.from_bytes(self._read(2, 5.0), "little")
        credit = window
        sent = 0
        while sent < len(data):
            # \x01 grants another window; \x04 means the watch has given up on the input.
            while credit == 0 or len(self.port.buffer) > self._mark:
                flow = self._read(1, 10.0)
                if flow == b"\x01":
                    credit += window
                elif flow == b"\x04":
                    self._write(b"\x04")
                    return
            chunk = data[sent : sent + credit]
            self._write(chunk)
            sent += len(chunk)
            credit -= len(chunk)
        self._write(b"\x04")
        while self._read(1, 10.0) != b"\x04":
            pass

    def run(
        self,
        code: str,
        timeout: Optional[float] = None,
        on_output: Optional[Callable[[bytes], None]] = None,
    ) -> str:
        """Runs code to completion, as start() followed by wait()."""
        self.start(code)
        return self.wait(timeout, on_output)

    def _restart_since(self, start: int) -> bool:
        return any(self.port.buffer.find(sign, start) >= 0 for sign in (_BOOT_BANNER, _RAW_PROMPT))

    def wait(
        self,
        timeout: Optional[float] = None,
        on_output: Optional[Callable[[bytes], None]] = None,
    ) -> str:
        """
        Waits for started code to finish, and returns what it printed. Raises PythonError if
        the code raises, WatchRestarted if it never gets to finish, TimeoutError on timeout.
        """
        # Execution output ends at the first EOT, its error text at the second.
        deadline = None if timeout is None else time.monotonic() + timeout
        out = bytearray()
        while True:
            remaining = 1.0 if deadline is None else max(0.0, deadline - time.monotonic())
            start = self._mark
            self.port.wait_for(lambda b: len(b) > start, min(remaining, 0.05))
            chunk = bytes(self.port.buffer[start:])
            end = chunk.find(b"\x04")
            piece = chunk if end < 0 else chunk[:end]
            if piece:
                out.extend(piece)
                if on_output:
                    on_output(piece)
            self._mark = start + len(piece)
            if end >= 0:
                self._mark += 1
                break
            if self._restart_since(self._started):
                panicked = self.port.buffer.find(b"Guru Meditation", self._started) >= 0
                self._mark = len(self.port.buffer)
                raise WatchRestarted(
                    "the watch panicked and restarted" if panicked else "the watch restarted"
                )
            if deadline is not None and time.monotonic() > deadline:
                self.interrupt()
                raise TimeoutError(f"script still running after {timeout} s")
            if not self.watch.running:
                raise TimeoutError(
                    f"the emulator exited while the script was running: {self.watch.emulator_log()}"
                )

        error = self._wait(b"\x04", 5.0)
        self._wait(b">", 5.0)
        if error:
            raise PythonError(error.decode(errors="replace").strip())
        return out.decode(errors="replace")

    def interrupt(self) -> None:
        self._write(b"\x03")

    def write_file(self, name: str, content: bytes) -> None:
        """Stores a file on the watch's filesystem, such as main.py."""
        self.run(f"f = open({name!r}, 'wb')")
        for i in range(0, len(content), 200):
            self.run(f"f.write({content[i : i + 200]!r})")
        self.run("f.close()")

    def soft_reset(self) -> None:
        """Leaves raw mode and restarts MicroPython, which then runs main.py."""
        self._write(b"\x02\x04")

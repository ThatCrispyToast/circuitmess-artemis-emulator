"""Runs Python on the watch over the serial REPL, the way mpremote and CircuitBlocks do."""

from __future__ import annotations

import time
from typing import Callable, Optional

from .machine import EmulatorError, Watch

_RAW_PROMPT = b"raw REPL; CTRL-B to exit\r\n>"
_BOOT_BANNER = b"ESP-ROM:"


class PythonError(Exception):
    """A script raised an exception on the watch."""

    def __init__(self, traceback: str):
        super().__init__(traceback)
        self.traceback = traceback


class WatchRestarted(Exception):
    """The watch rebooted before the script finished, taking the script with it."""


class PythonSession:
    """Talks to MicroPython's raw REPL on the watch's USB serial port."""

    def __init__(self, watch: Watch, boot_timeout: float = 60.0):
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

    def _write(self, data: bytes) -> None:
        try:
            self.port.write(data)
        except OSError:
            raise EmulatorError(f"the emulator stopped: {self.watch.emulator_log()}") from None

    def _enter_raw(self, timeout: float) -> None:
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
                    raise TimeoutError(
                        "MicroPython's REPL never answered: is this a MicroPython firmware?"
                    ) from None

    def start(self, code: str) -> None:
        """Starts code without waiting for it, to interact with it or if it restarts the watch."""
        self._mark = len(self.port.buffer)
        data = code.encode()
        for i in range(0, len(data), 256):
            self._write(data[i : i + 256])
            time.sleep(0.001)
        self._write(b"\x04")
        self._wait(b"OK", 10.0)
        self._started = self._mark

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

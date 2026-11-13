"""Client side of the bridge protocol documented in qemu/overlay/hw/artemis/board.c."""

from __future__ import annotations

import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Callable, Optional

PROTOCOL_VERSION = 2

# What SET_RTC carries for a clock that has never been set.
RTC_NEVER_SET = -(1 << 63)


class Msg(IntEnum):
    HELLO = 0x01
    FRAME = 0x02
    PIN = 0x03
    POWER = 0x04
    SYNCED = 0x05

    SET_PIN = 0x81
    SET_IMU = 0x82
    SET_BATTERY = 0x83
    SET_RTC = 0x84
    RESET = 0x85
    REQ_FRAME = 0x86
    TAP = 0x87
    STEPS = 0x88
    SYNC = 0x89


@dataclass
class PinState:
    level: int = 0  # duty, 0-65535
    freq: int = 0  # Hz, 0 when not PWM


@dataclass
class BoardState:
    """What the bridge knows about the hardware right now."""

    revision: int = 0
    width: int = 128
    height: int = 128
    powered: bool = True
    frame: bytes = b""
    frame_seq: int = 0
    pins: dict[int, PinState] = field(default_factory=dict)


class Bridge:
    """Connects to the board device and tracks its state on a reader thread."""

    def __init__(self, path: str, connect_timeout: float = 10.0):
        self.state = BoardState()
        self._cond = threading.Condition()
        self._send_lock = threading.Lock()
        self._listeners: list[Callable[[Msg], None]] = []
        self._closed = False
        self._syncs_sent = 0
        self._syncs_seen = 0

        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.monotonic() + connect_timeout
        while True:
            try:
                self._sock.connect(path)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise TimeoutError(f"bridge socket {path} never appeared") from None
                time.sleep(0.02)

        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()

    def add_listener(self, fn: Callable[[Msg], None]) -> None:
        self._listeners.append(fn)

    # ------------------------------------------------------------------
    # Commands

    def _send(self, kind: Msg, payload: bytes = b"") -> None:
        n = len(payload)
        with self._send_lock:
            self._sock.sendall(bytes((kind, n & 0xFF, n >> 8 & 0xFF, n >> 16 & 0xFF)) + payload)

    def set_pin(self, pin: int, level: int) -> None:
        """level is 0, 1, or -1 to release the pad."""
        self._send(Msg.SET_PIN, struct.pack("<Bb", pin, level))

    def set_imu(
        self,
        accel_mg: tuple[float, float, float],
        gyro_mdps: tuple[float, float, float] = (0.0, 0.0, 0.0),
        temp_c: float = 25.0,
    ) -> None:
        self._send(Msg.SET_IMU, struct.pack("<7f", *accel_mg, *gyro_mdps, temp_c))

    def set_battery(self, mv: int, usb: bool = False, charging: bool = False) -> None:
        self._send(Msg.SET_BATTERY, struct.pack("<HBB", mv, usb, charging))

    def set_rtc(self, epoch: Optional[int]) -> None:
        """None puts the clock in the state it powers up in: time lost, counting from zero."""
        self._send(Msg.SET_RTC, struct.pack("<q", RTC_NEVER_SET if epoch is None else epoch))

    def reset(self) -> None:
        self._send(Msg.RESET)

    def request_frame(self) -> None:
        self._send(Msg.REQ_FRAME)

    def tap(self, axis: int = 2, double: bool = False) -> None:
        self._send(Msg.TAP, bytes((axis, double)))

    def steps(self, count: int) -> None:
        self._send(Msg.STEPS, struct.pack("<H", count))

    def sync(self, timeout: float = 5.0) -> None:
        """
        Returns once the board has applied every command sent so far, and everything
        it had reported by then has reached self.state.
        """
        with self._cond:
            self._syncs_sent += 1
            token = self._syncs_sent
        try:
            self._send(Msg.SYNC, struct.pack("<I", token))
        except OSError:
            return  # the emulator has gone; what we last heard is all there is
        with self._cond:
            self._cond.wait_for(lambda: self._closed or self._syncs_seen >= token, timeout)

    # ------------------------------------------------------------------
    # Waiting

    def wait_for(self, predicate: Callable[[BoardState], bool], timeout: float) -> bool:
        with self._cond:
            return self._cond.wait_for(lambda: predicate(self.state), timeout)

    def next_frame(self, timeout: float = 5.0) -> bytes:
        """Blocks until a frame newer than the current one has arrived."""
        with self._cond:
            seq = self.state.frame_seq
            if not self._cond.wait_for(lambda: self.state.frame_seq != seq, timeout):
                raise TimeoutError("no new frame from the display")
            return self.state.frame

    # ------------------------------------------------------------------

    def _recv_exact(self, n: int) -> bytes | None:
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self._sock.recv(n - len(buf))
            except OSError:
                return None
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)

    def _reader(self) -> None:
        while not self._closed:
            header = self._recv_exact(4)
            if header is None:
                break
            kind = header[0]
            length = header[1] | header[2] << 8 | header[3] << 16
            payload = self._recv_exact(length) if length else b""
            if payload is None:
                break
            with self._cond:
                self._apply(kind, payload)
                self._cond.notify_all()
            for fn in self._listeners:
                fn(Msg(kind))

        with self._cond:
            self._closed = True
            self._cond.notify_all()

    def _apply(self, kind: int, p: bytes) -> None:
        s = self.state
        if kind == Msg.HELLO:
            version, s.revision, s.width, s.height = struct.unpack_from("<HBBB", p)
            if version != PROTOCOL_VERSION:
                raise RuntimeError(f"unsupported bridge protocol {version}")
        elif kind == Msg.FRAME:
            s.frame_seq = struct.unpack_from("<I", p)[0]
            s.frame = p[4:]
        elif kind == Msg.PIN:
            pin, level, freq = struct.unpack_from("<BHI", p)
            s.pins[pin] = PinState(level, freq)
        elif kind == Msg.POWER:
            s.powered = bool(p[0])
        elif kind == Msg.SYNCED:
            self._syncs_seen = struct.unpack_from("<I", p)[0]

"""Emulator for the CircuitMess NASA Artemis Watch 2.0."""

from .flash import FirmwareError
from .machine import EmulatorError, Watch
from .micropython import PythonError, PythonSession, WatchRestarted

__all__ = [
    "EmulatorError",
    "FirmwareError",
    "PythonError",
    "PythonSession",
    "Watch",
    "WatchRestarted",
]

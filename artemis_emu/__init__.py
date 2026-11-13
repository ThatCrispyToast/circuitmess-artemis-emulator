"""Emulator for the CircuitMess NASA Artemis Watch 2.0, and tools for the real one."""

from .device import Device, DeviceError
from .firmware import STOCK_VERSIONS, micropython_firmware, stock_firmware
from .machine import BUTTONS, EmulatorError, Watch
from .micropython import PythonError, PythonSession, WatchRestarted

__all__ = [
    "BUTTONS",
    "Device",
    "DeviceError",
    "EmulatorError",
    "PythonError",
    "PythonSession",
    "STOCK_VERSIONS",
    "Watch",
    "WatchRestarted",
    "micropython_firmware",
    "stock_firmware",
]

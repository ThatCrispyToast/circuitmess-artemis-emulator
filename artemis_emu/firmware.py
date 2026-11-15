"""Finds the firmware images that come with the emulator, and says what an image is."""

from __future__ import annotations

import os
import struct
from pathlib import Path
from typing import Optional

from . import assets
from .flash import APP_OFFSET, PARTITION_MAGIC, PARTITION_TABLE_OFFSET

_RELEASES = assets.PINS["stock"]["releases"]

# CircuitMess's releases for this watch, oldest first. 2.0 is what hardware revision 2
# left the factory with; revision 3 needs 2.1 or later.
STOCK_VERSIONS = tuple(_RELEASES)

_APP_DESC_MAGIC = 0xABCD5432


class FirmwareNotFound(Exception):
    pass


def stock_firmware(version: Optional[str] = None) -> Path:
    """The factory firmware: the latest release, or the one asked for."""
    if version is None:
        named = os.environ.get("ARTEMIS_STOCK_FIRMWARE")
        if named:
            return Path(named)
        version = STOCK_VERSIONS[-1]
    version = version.lstrip("v")
    if version not in STOCK_VERSIONS:
        raise FirmwareNotFound(
            f"no stock firmware {version}: the releases are {', '.join(STOCK_VERSIONS)}"
        )
    directory = os.environ.get("ARTEMIS_STOCK_FIRMWARE_DIR")
    if directory:
        return Path(directory) / f"Artemis-v{version}.bin"
    try:
        return assets.stock_firmware(version)
    except assets.AssetError as error:
        raise FirmwareNotFound(str(error)) from None


def micropython_firmware() -> Path:
    """CircuitMess's MicroPython, as this project builds it."""
    named = os.environ.get("ARTEMIS_MICROPYTHON_FIRMWARE")
    if named:
        return Path(named)
    try:
        return assets.micropython_firmware()
    except assets.AssetError as error:
        raise FirmwareNotFound(str(error)) from None


def partitions(image: bytes) -> list[tuple[str, int, int, int, int]]:
    """(label, type, subtype, offset, size) for each entry of a flash image's partition table."""
    entries = []
    for at in range(PARTITION_TABLE_OFFSET, PARTITION_TABLE_OFFSET + 0xC00, 32):
        entry = image[at : at + 32]
        if len(entry) < 32 or struct.unpack_from("<H", entry)[0] != PARTITION_MAGIC:
            break
        kind, subtype, offset, size = struct.unpack_from("<BBII", entry, 2)
        label = entry[12:28].split(b"\0")[0].decode(errors="replace")
        entries.append((label, kind, subtype, offset, size))
    return entries


def _elf_hash(image: bytes) -> bytes:
    """The hash of the build an image's application came from, kept in its descriptor."""
    desc = image[APP_OFFSET + 32 : APP_OFFSET + 32 + 256]
    if len(desc) < 256 or struct.unpack_from("<I", desc)[0] != _APP_DESC_MAGIC:
        return b""
    return desc[144:176]


def stock_release(image: bytes) -> Optional[str]:
    """Which CircuitMess release an image's application is, if it is one."""
    wanted = _elf_hash(image).hex()
    for version, release in _RELEASES.items():
        if release["elf"] == wanted:
            return version
    return None


def describe(image: bytes) -> str:
    """What firmware a flash image holds."""
    desc = image[APP_OFFSET + 32 : APP_OFFSET + 32 + 256]
    if len(desc) < 256 or struct.unpack_from("<I", desc)[0] != _APP_DESC_MAGIC:
        return "no application"

    def text(offset: int, length: int) -> str:
        return desc[offset : offset + length].split(b"\0")[0].decode(errors="replace")

    project, version = text(48, 32), text(16, 32)
    built = f"{text(96, 16)} {text(80, 16)}"
    if project == "micropython":
        return f"MicroPython (built {built}, ESP-IDF {text(112, 32)})"
    if project == "Artemis-Firmware":
        release = stock_release(image)
        if release:
            return f"CircuitMess stock firmware {release}"
        return f"CircuitMess stock firmware, not a published release (source {version}, built {built})"
    name = f"{project} {version}".strip() or "an unnamed application"
    return f"{name} (built {built}, ESP-IDF {text(112, 32)})"

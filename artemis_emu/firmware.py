"""Finds the firmware images that come with the emulator, and says what an image is."""

from __future__ import annotations

import os
import struct
from pathlib import Path
from typing import Optional

from .flash import APP_OFFSET, PARTITION_MAGIC, PARTITION_TABLE_OFFSET

# CircuitMess's releases for this watch, oldest first. 2.0 is what hardware revision 2
# left the factory with; revision 3 needs 2.1 or later.
STOCK_VERSIONS = ("2.0", "2.1", "2.1.1", "2.2")

_APP_DESC_MAGIC = 0xABCD5432


class FirmwareNotFound(Exception):
    pass


def _from_env(variable: str, what: str) -> Path:
    path = os.environ.get(variable)
    if not path:
        raise FirmwareNotFound(f"{what} isn't available: run through `nix run`, or set {variable}")
    return Path(path)


def stock_firmware(version: Optional[str] = None) -> Path:
    """The factory firmware: the latest release, or the one asked for."""
    if version is None:
        return _from_env("ARTEMIS_STOCK_FIRMWARE", "The stock firmware")
    version = version.lstrip("v")
    if version not in STOCK_VERSIONS:
        raise FirmwareNotFound(
            f"no stock firmware {version}: the releases are {', '.join(STOCK_VERSIONS)}"
        )
    directory = _from_env("ARTEMIS_STOCK_FIRMWARE_DIR", "That stock firmware")
    return directory / f"Artemis-v{version}.bin"


def micropython_firmware() -> Path:
    return _from_env("ARTEMIS_MICROPYTHON_FIRMWARE", "The MicroPython firmware")


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
    wanted = _elf_hash(image)
    directory = os.environ.get("ARTEMIS_STOCK_FIRMWARE_DIR")
    if not wanted or not directory:
        return None
    for version in STOCK_VERSIONS:
        path = Path(directory) / f"Artemis-v{version}.bin"
        if path.exists():
            with open(path, "rb") as release:
                if _elf_hash(release.read(APP_OFFSET + 0x200)) == wanted:
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

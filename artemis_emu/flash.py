"""Turns what a build produces into the 4 MB flash image the watch boots from."""

from __future__ import annotations

import json
import struct
from pathlib import Path

FLASH_SIZE = 4 * 1024 * 1024
ESP_IMAGE_MAGIC = 0xE9
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_MAGIC = 0x50AA
APP_OFFSET = 0x10000


class FirmwareError(Exception):
    pass


def _has_partition_table(data: bytes) -> bool:
    if len(data) < PARTITION_TABLE_OFFSET + 32:
        return False
    return struct.unpack_from("<H", data, PARTITION_TABLE_OFFSET)[0] == PARTITION_MAGIC


def _pad(data: bytes) -> bytes:
    if len(data) > FLASH_SIZE:
        raise FirmwareError(
            f"image is {len(data)} bytes, but the watch has {FLASH_SIZE} bytes of flash"
        )
    return data + b"\xff" * (FLASH_SIZE - len(data))


def _from_idf_build(build_dir: Path) -> bytes:
    """Combines the files `idf.py flash` would write, per flasher_args.json."""
    args = json.loads((build_dir / "flasher_args.json").read_text())
    image = bytearray(b"\xff" * FLASH_SIZE)
    for offset, name in args["flash_files"].items():
        data = (build_dir / name).read_bytes()
        start = int(offset, 16)
        if start + len(data) > FLASH_SIZE:
            raise FirmwareError(f"{name} doesn't fit at {offset}")
        image[start : start + len(data)] = data
    return bytes(image)


def _from_app_image(app: bytes, base_image: bytes | None) -> bytes:
    """Places a bare application behind a bootloader and partition table."""
    if base_image is None or not _has_partition_table(base_image):
        raise FirmwareError(
            "this is a bare application image, and needs a bootloader and partition "
            "table from somewhere: pass a merged image"
        )

    image = bytearray(_pad(base_image[: APP_OFFSET]))
    if APP_OFFSET + len(app) > FLASH_SIZE:
        raise FirmwareError("application doesn't fit in flash")
    image[APP_OFFSET : APP_OFFSET + len(app)] = app
    return bytes(image)


def flash_plan(source: str | Path) -> list[tuple[int, Path]]:
    """What to write where to put a build on a real watch: (offset, file) pairs."""
    path = Path(source)

    if path.is_dir():
        for build_dir in (path, path / "build"):
            if (build_dir / "flasher_args.json").exists():
                args = json.loads((build_dir / "flasher_args.json").read_text())
                return [(int(offset, 16), build_dir / name)
                        for offset, name in sorted(args["flash_files"].items())]
        raise FirmwareError(f"{path} has no flasher_args.json: not an ESP-IDF build directory")

    data = path.read_bytes()
    if not data or data[0] != ESP_IMAGE_MAGIC:
        raise FirmwareError(f"{path} isn't an ESP32 firmware image")
    if len(data) > FLASH_SIZE:
        raise FirmwareError(f"{path} is larger than the watch's {FLASH_SIZE} bytes of flash")
    # A bare application goes where the partition table expects it.
    return [(0 if _has_partition_table(data) else APP_OFFSET, path)]


def build_flash_image(source: str | Path, base_image=None) -> bytes:
    """
    Accepts a merged flash image, a bare app image, or an ESP-IDF build directory.
    A bare app goes behind the bootloader and partition table of `base_image`: a
    path, or a function that gives one, called only if it comes to that.
    """
    path = Path(source)

    if path.is_dir():
        for build_dir in (path, path / "build"):
            if (build_dir / "flasher_args.json").exists():
                return _from_idf_build(build_dir)
        raise FirmwareError(f"{path} has no flasher_args.json: not an ESP-IDF build directory")

    data = path.read_bytes()
    if not data:
        raise FirmwareError(f"{path} is empty")
    if path.suffix == ".elf" or data[:4] == b"\x7fELF":
        raise FirmwareError(
            f"{path} is an ELF file. Give the emulator the flashable .bin or the build directory"
        )
    if data[0] != ESP_IMAGE_MAGIC:
        raise FirmwareError(f"{path} isn't an ESP32 firmware image")

    if _has_partition_table(data):
        return _pad(data)

    base = base_image() if callable(base_image) else base_image
    return _from_app_image(data, Path(base).read_bytes() if base else None)

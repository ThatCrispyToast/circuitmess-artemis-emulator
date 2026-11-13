"""Turns RGB565 frames into RGB888 and PNG, using only the standard library."""

from __future__ import annotations

import struct
import zlib
from array import array


def rgb565_to_rgb888(frame: bytes) -> bytes:
    """Expects little-endian RGB565, as the bridge sends it."""
    words = array("H")
    words.frombytes(frame)
    out = bytearray(len(words) * 3)
    for i, c in enumerate(words):
        r = c >> 11
        g = c >> 5 & 0x3F
        b = c & 0x1F
        out[i * 3] = r << 3 | r >> 2
        out[i * 3 + 1] = g << 2 | g >> 4
        out[i * 3 + 2] = b << 3 | b >> 2
    return bytes(out)


def encode_png(rgb: bytes, width: int, height: int, scale: int = 1) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data)
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    rows = bytearray()
    for y in range(height):
        row = rgb[y * width * 3 : (y + 1) * width * 3]
        if scale > 1:
            row = b"".join(row[i : i + 3] * scale for i in range(0, len(row), 3))
        for _ in range(scale):
            rows.append(0)
            rows.extend(row)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(rows), 6))
        + chunk(b"IEND", b"")
    )


def save_png(path: str, frame: bytes, width: int = 128, height: int = 128, scale: int = 1) -> None:
    with open(path, "wb") as f:
        f.write(encode_png(rgb565_to_rgb888(frame), width, height, scale))

"""Builds the eFuse image a factory-programmed Artemis Watch would carry."""

BLOCK_OFFSET = {0: 0, 1: 6, 2: 12, 3: 20}
IMAGE_BYTES = 1024

PRODUCT_ID = 0x0003
DEFAULT_REVISION = 3

# Locally administered, so it can't collide with a real device.
DEFAULT_MAC = bytes.fromhex("02a7eb152026")

# What a production watch holds, read from one, less what identifies it: the MAC
# and the chip's unique ID. Block 0 selects the flash supply; block 1 carries the
# chip revision (v0.2), package (4 MB flash, 2 MB PSRAM) and regulator trims; block 2
# the ADC and temperature sensor calibration. The ADC figures work out to codes of
# 3659, 2742, 1923 and 1017 at 850 mV for the four attenuations, which is what
# adc_digi_at_850mv in qemu/overlay/hw/artemis/board.c makes the converter produce.
FACTORY_WORDS = {
    (0, 2): 0x0000D1F5,
    (1, 3): 0x54080000,
    (1, 4): 0x4C2088B1,
    (1, 5): 0x6C00C418,
    (2, 4): 0x52CAF5A1,
    (2, 5): 0x3152BED4,
    (2, 6): 0xD4EEEAD2,
    (2, 7): 0x3C68E8EA,
}


def _set_field(words: list[int], block: int, bit: int, width: int, value: int) -> None:
    if value >= 1 << width:
        raise ValueError(f"{value} doesn't fit in {width} bits")
    base = BLOCK_OFFSET[block]
    for i in range(width):
        word = base + (bit + i) // 32
        if value >> i & 1:
            words[word] |= 1 << ((bit + i) % 32)


def build_efuse(revision: int = DEFAULT_REVISION, mac: bytes = DEFAULT_MAC) -> bytes:
    words = [0] * (IMAGE_BYTES // 4)

    for (block, index), value in FACTORY_WORDS.items():
        words[BLOCK_OFFSET[block] + index] = value

    # Hardware identity the firmware and bootloader hook check (EfuseMeta).
    _set_field(words, 3, 16, 16, PRODUCT_ID)
    _set_field(words, 3, 32, 8, revision)

    # The factory MAC is stored most significant byte first.
    for i, byte in enumerate(reversed(mac)):
        _set_field(words, 1, i * 8, 8, byte)

    return b"".join(w.to_bytes(4, "little") for w in words)

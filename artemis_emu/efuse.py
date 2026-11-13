"""Builds the eFuse image a factory-programmed Artemis Watch would carry."""

BLOCK_WORDS = {0: 6, 1: 6, 2: 8, 3: 8}
BLOCK_OFFSET = {0: 0, 1: 6, 2: 12, 3: 20}
IMAGE_BYTES = 1024

PRODUCT_ID = 0x0003
DEFAULT_REVISION = 3

# ADC codes the board model produces at 850 mV for each attenuation; these have
# to agree with adc_digi_at_850mv in qemu/overlay/hw/artemis/board.c.
ADC_DIGI_AT_850MV = (3664, 2785, 1989, 1123)

# Locally administered, so it can't collide with a real device.
DEFAULT_MAC = bytes.fromhex("02a7eb152026")


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

    # Hardware identity the firmware and bootloader hook check (EfuseMeta).
    _set_field(words, 3, 16, 16, PRODUCT_ID)
    _set_field(words, 3, 32, 8, revision)

    # The factory MAC is stored most significant byte first.
    for i, byte in enumerate(reversed(mac)):
        _set_field(words, 1, i * 8, 8, byte)

    # Block 2 calibration, version 1. Without it adc_cali_* refuses to start and
    # the stock firmware aborts. The fields are deltas: see
    # esp_efuse_rtc_calib_get_cal_voltage() in ESP-IDF.
    _set_field(words, 2, 128, 2, 1)  # BLK_VERSION_MAJOR

    a0, a1, a2, a3 = ADC_DIGI_AT_850MV
    _set_field(words, 2, 201, 8, a0 - a1 - 800)  # ADC1_CAL_VOL_ATTEN0
    _set_field(words, 2, 209, 8, a1 - a2 - 700)  # ADC1_CAL_VOL_ATTEN1
    _set_field(words, 2, 217, 8, a2 - a3 - 800)  # ADC1_CAL_VOL_ATTEN2
    _set_field(words, 2, 225, 8, a3 - 900)  # ADC1_CAL_VOL_ATTEN3
    # ADC2 stored relative to ADC1 with a fixed bias: these make them equal.
    _set_field(words, 2, 233, 8, 40)  # ADC2_CAL_VOL_ATTEN0
    _set_field(words, 2, 241, 7, 10)  # ADC2_CAL_VOL_ATTEN1
    _set_field(words, 2, 248, 7, 20)  # ADC2_CAL_VOL_ATTEN2
    _set_field(words, 1, 186, 6, 15)  # ADC2_CAL_VOL_ATTEN3

    return b"".join(w.to_bytes(4, "little") for w in words)

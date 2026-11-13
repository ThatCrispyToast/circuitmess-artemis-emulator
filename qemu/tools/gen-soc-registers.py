#!/usr/bin/env python3
"""
Generates qemu/overlay/hw/artemis/soc_registers.h from ESP-IDF's register headers:
for each register of the listed ESP32-S3 blocks, its address, its value out of reset,
and which of its bits simply hold what firmware writes.

    gen-soc-registers.py /path/to/esp-idf > qemu/overlay/hw/artemis/soc_registers.h
"""

import re
import sys
from pathlib import Path

# Header, the base its registers are written against, and where instances sit.
BLOCKS = [
    ("rtc_cntl_reg.h", "DR_REG_RTCCNTL_BASE", [0x60008000]),
    ("rtc_io_reg.h", "DR_REG_RTCIO_BASE", [0x60008400]),
    ("sens_reg.h", "DR_REG_SENS_BASE", [0x60008800]),
    ("spi_mem_reg.h", "REG_SPI_MEM_BASE(i)", [0x60003000]),
    ("i2s_reg.h", "REG_I2S_BASE(i)", [0x6000F000, 0x6002D000]),
    ("uhci_reg.h", "REG_UHCI_BASE(i)", [0x60014000]),
    ("rmt_reg.h", "DR_REG_RMT_BASE", [0x60016000]),
    ("pcnt_reg.h", "DR_REG_PCNT_BASE", [0x60017000]),
    ("syscon_reg.h", "DR_REG_SYSCON_BASE", [0x60026000]),
    ("apb_saradc_reg.h", "DR_REG_APB_SARADC_BASE", [0x60040000]),
    ("lcd_cam_reg.h", "DR_REG_LCD_CAM_BASE", [0x60041000]),
    ("usb_wrap_reg.h", "DR_REG_USB_WRAP_BASE", [0x60039000]),
    ("peri_backup_reg.h", "DR_REG_PERI_BACKUP_BASE", [0x6002A000]),
    ("sensitive_reg.h", "DR_REG_SENSITIVE_BASE", [0x600C1000]),
    ("extmem_reg.h", "DR_REG_EXTMEM_BASE", [0x600C4000]),
    ("assist_debug_reg.h", "DR_REG_ASSIST_DEBUG_BASE", [0x600CE000]),
    ("world_controller_reg.h", "DR_REG_WORLD_CNTL_BASE", [0x600D0000]),
]

# Fields firmware can set and read back. Self-clearing and write-only ones read as 0.
STORED = {"R/W", "RW", "R/W/SS"}
# Fields that can't be read have no value out of reset either.
UNREADABLE = {"WO", "WT", "WOD"}

# Bits the headers call read/write that don't take writes on a watch.
READ_ONLY = {
    0x60003018: 0x50000000,
    0x60003024: 0xffffffff,
    0x60003028: 0xffffffff,
    0x60003034: 0xffffffff,
}

# Where a watch's registers, read back after it had booted, differ from what the
# headers say they reset to: the silicon is the authority, so these win.
MEASURED = {
    0x60003008: 0x012c2008,
    0x6000300c: 0x00000000,
    0x60003010: 0x00002c00,
    0x60003014: 0x80000000,
    0x60003018: 0x200000c0,
    0x6000301c: 0x5c000005,
    0x60003020: 0x700000eb,
    0x60003034: 0x00000000,
    0x60003038: 0x00000000,
    0x60003024: 0x00000000,
    0x60003028: 0x00000000,
    0x6000303c: 0x00000005,
    0x60003098: 0x00000000,
    0x600030a0: 0x00000000,
    0x600030dc: 0x0000b107,
    0x600030e0: 0x00000000,
    0x600030e8: 0x00000000,
    0x600030f8: 0x00000004,
    0x60008018: 0x20000000,
    0x6000801c: 0x14094143,
    0x60008024: 0x02010201,
    0x60008028: 0x02010201,
    0x6000802c: 0x00000200,
    0x60008030: 0x02010201,
    0x60008034: 0x00400000,
    0x6000803c: 0x00040000,
    0x60008044: 0x00020000,
    0x60008060: 0x00076e80,
    0x60008068: 0xc0000000,
    0x60008080: 0x003bc800,
    0x60008084: 0x103fc000,
    0x60008088: 0x00100000,
    0x6000808c: 0x0000f000,
    0x60008090: 0xc0020000,
    0x600080b8: 0x00000000,
    0x600080d0: 0x08150730,
    0x600080d4: 0x02719100,
    0x600080e8: 0x4bffc020,
    0x60008108: 0x01f4000f,
    0x6000810c: 0x81fe70c8,
    0x60008110: 0xf03c0102,
    0x60008140: 0xfefe4000,
    0x60008148: 0x00000000,
    0x6000814c: 0x000000fc,
    0x60008150: 0xfc000000,
    0x60008804: 0x20000000,
    0x6000885c: 0xfff47fff,
    0x600088e4: 0x70000000,
    0x60017000: 0x00003c10,
    0x6001700c: 0x00003c10,
    0x60017018: 0x00003c10,
    0x60017024: 0x00003c10,
    0x60017060: 0x00000055,
    0x60026014: 0xffffdfdf,
    0x60026018: 0x00000000,
    0x6002608c: 0xffffffff,
    0x60026098: 0x80000000,
    0x6002609c: 0x00000040,
    0x60039000: 0x00000000,
    0x600c4144: 0x00000009,
    0x600ce010: 0x03fffff0,
    0x600ce018: 0x03fffff0,
    0x600ce020: 0xffffffff,
    0x600ce028: 0xffffffff,
    0x600ce030: 0x00000000,
    0x600ce038: 0x0000000b,
    0x600ce040: 0xffffffff,
}

REGISTER = re.compile(r"#define\s+(\w+_REG)(?:\(i\))?\s+\((.+?)\s*\+\s*(0x[0-9A-Fa-f]+|\d+)\s*\)")
FIELD = re.compile(r"/\*+\s*(\w+)\s*:\s*([\w/]+)\s*;\s*bitpos:\s*\[(\d+)(?::(\d+))?\]\s*;\s*default:\s*"
                   r"(~)?\s*(?:(\d+)'([bhd]))?\s*([0-9a-fA-FxX]+)")


def parse(path: Path, base: str):
    registers = []
    current = None
    for line in path.read_text().splitlines():
        match = REGISTER.match(line)
        if match:
            name, expression, offset = match.groups()
            current = None
            if expression.replace(" ", "") == base.replace(" ", ""):
                current = [name, int(offset, 0), 0, 0]
                registers.append(current)
            continue
        match = FIELD.match(line.strip())
        if match and current:
            _, access, high, low, inverted, _, radix, value = match.groups()
            high = int(high)
            low = int(low) if low is not None else high
            mask = ((1 << (high - low + 1)) - 1) << low
            if value.lower().startswith("0x"):
                number = int(value, 16)
            else:
                number = int(value, {"b": 2, "h": 16, "d": 10, None: 10}[radix])
            if inverted:
                number = ~number
            if access not in UNREADABLE:
                current[2] |= (number << low) & mask & 0xFFFFFFFF
            if access in STORED:
                current[3] |= mask
    return registers


def main():
    soc = Path(sys.argv[1]) / "components/soc/esp32s3/include/soc"
    version = (Path(sys.argv[1]) / "version.txt")
    print("/*")
    print(" * Reset values and writable bits of ESP32-S3 registers, from the register")
    print(" * descriptions in ESP-IDF (Apache-2.0). Generated by qemu/tools/gen-soc-registers.py;")
    print(" * don't edit by hand.")
    print(" */")
    print("static const ArtemisSocRegister artemis_soc_registers[] = {")
    count = 0
    measured = dict(MEASURED)
    for header, base, instances in BLOCKS:
        registers = parse(soc / header, base)
        for address in instances:
            print(f"    /* {header} at {address:#010x} */")
            seen = set()
            for name, offset, reset, writable in registers:
                if offset in seen:
                    continue
                seen.add(offset)
                reset = measured.pop(address + offset, reset)
                writable &= ~READ_ONLY.get(address + offset, 0)
                print(f"    {{ {address + offset:#010x}, {reset:#010x}, {writable:#010x} }},")
                count += 1
    if measured:
        print("    /* registers the headers leave out */")
        for address, reset in sorted(measured.items()):
            print(f"    {{ {address:#010x}, {reset:#010x}, 0xffffffff }},")
            count += 1
    print("};")
    print(f"/* {count} registers */")


main()

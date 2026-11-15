"""
What a real watch does, which the emulator has to do too.

The figures here were measured on a watch (hardware revision 2, on USB, lying flat) running
this project's MicroPython firmware. To run the same tests on a watch instead, plug one in
with that firmware on it and set ARTEMIS_TEST_ON_WATCH=1 (and ARTEMIS_WATCH_PORT if several
are plugged in):

    artemis-emu device flash micropython
    ARTEMIS_TEST_ON_WATCH=1 python -m pytest tests/test_like_a_watch.py
"""

import ast
import os
from pathlib import Path

import pytest

from artemis_emu import Device, PythonSession, Watch
from artemis_emu.micropython import WatchRestarted

MICROPYTHON = os.environ.get("ARTEMIS_MICROPYTHON_FIRMWARE")
ON_WATCH = bool(os.environ.get("ARTEMIS_TEST_ON_WATCH"))

# How the watch sat while it was measured: gravity in g, its gyroscope's offsets in
# degrees a second, and its temperature.
RESTING = ((0.015, -0.506, 0.926), (2.0, -4.4, -4.8), 26.0)


# Has the RTC watchdog reset the whole chip, which is as close to a power cycle as a watch
# on USB gets: a plain reset leaves the RTC domain as earlier code left it.
CHIP_RESET = """
from machine import mem32
mem32[0x600080B0] = 0x50D83AA1                              # unlock the watchdog
mem32[0x600080AC] = 0x80000000                              # feed it
mem32[0x6000809C] = 3000                                    # 20 ms
mem32[0x60008098] = 1 << 31 | 4 << 28 | 1 << 16 | 1 << 13   # on, stage 0 resets chip and RTC
import time
time.sleep(1)
"""


def fresh_watch(device):
    """A session on a real watch that has just been through a whole-chip reset."""
    device.reset()
    device.wait_for_output("MicroPython v", 15)
    try:
        PythonSession(device, boot_timeout=30).run(CHIP_RESET, timeout=5)
    except (WatchRestarted, TimeoutError, OSError):
        pass
    # It drops off USB and comes back.
    device.reconnect(timeout=20)
    return PythonSession(device, boot_timeout=30)


@pytest.fixture
def python():
    if ON_WATCH:
        with Device(os.environ.get("ARTEMIS_WATCH_PORT") or None) as device:
            yield fresh_watch(device)
    elif not MICROPYTHON:
        pytest.skip("ARTEMIS_MICROPYTHON_FIRMWARE isn't set")
    else:
        with Watch(MICROPYTHON, revision=2, usb=True, battery_mv=4100) as watch:
            watch.set_motion(*RESTING)
            yield PythonSession(watch)


def measure(python, code, timeout=60):
    """Runs a script whose last line of output is a Python literal, and returns it."""
    return ast.literal_eval(python.run(code, timeout=timeout).strip().splitlines()[-1])


def within(value, expected, fraction):
    return abs(value - expected) <= expected * fraction


# ---------------------------------------------------------------------------------------
# How long code takes

WORKLOADS = """
import time, machine, gc
def loop(n):
    x = 0
    for i in range(n):
        x += i
def floats(n):
    s = 0.0
    for i in range(n):
        s += (i * 1.5) ** 0.5
def strings(n):
    parts = []
    for i in range(n):
        parts.append("%d:%s" % (i, hex(i)))
    ",".join(parts)
def calls(n):
    def f(a, b):
        return a + b
    x = 0
    for i in range(n):
        x = f(x, i)
def lists(n):
    data = [(i * 7919) % 1000 for i in range(n)]
    data.sort()
    d = {}
    for v in data:
        d[v] = d.get(v, 0) + 1
def drawing(n):
    import framebuf
    fb = framebuf.FrameBuffer(bytearray(128 * 128 * 2), 128, 128, framebuf.RGB565)
    for i in range(n):
        fb.fill(i)
        fb.fill_rect(10, 10, 100, 100, 0xFFFF)
        fb.text("Hello", 10, 10, 0)
        fb.line(0, 0, 127, 127, 0x1F)
def us(fn, n):
    gc.collect()
    t0 = time.ticks_us(); fn(n); return time.ticks_diff(time.ticks_us(), t0)
"""


@pytest.mark.parametrize("mhz, expected_us", [(240, 70840), (160, 106256), (80, 212645)])
def test_a_loop_takes_as_long_as_on_a_watch(python, mhz, expected_us):
    took = measure(python, WORKLOADS + f"""
machine.freq({mhz} * 1000000)
print(min(us(loop, 30000) for _ in range(3)))
""")
    assert within(took, expected_us, 0.06)


@pytest.mark.parametrize("workload, expected_us", [
    ("floats, 5000", 68251),
    ("strings, 2000", 715134),
    ("calls, 10000", 119982),
    ("lists, 2000", 52450),
    ("drawing, 20", 22185),
])
def test_other_work_takes_about_as_long_as_on_a_watch(python, workload, expected_us):
    took = measure(python, WORKLOADS + f"print(us({workload}))")
    # Work that leans on memory runs up to 8% fast: the chip's cache misses aren't modelled.
    assert within(took, expected_us, 0.12)


def test_the_cycle_counter_runs_at_the_cpu_frequency(python):
    rates = measure(python, """
import time, machine
out = []
for mhz in (160, 240, 80):
    machine.freq(mhz * 1000000)
    c0 = time.ticks_cpu(); t0 = time.ticks_us()
    time.sleep_ms(50)
    c1 = time.ticks_cpu(); t1 = time.ticks_us()
    out.append(time.ticks_diff(c1, c0) / time.ticks_diff(t1, t0))
machine.freq(160000000)
def steps(n):
    tc = time.ticks_cpu
    seen = []
    for _ in range(n):
        a = tc(); b = tc()
        seen.append(b - a)
    seen.sort()
    return seen[n // 2]
print((out, steps(200)))
""")
    rates, step = rates
    for rate, mhz in zip(rates, (160, 240, 80)):
        assert abs(rate - mhz) <= mhz * 0.002, rates
    # Read twice in a row it moves on by 160 cycles on a watch, the same every time.
    assert 150 <= step <= 185


REGISTER_LOOPS = """
import time, micropython
@micropython.viper
def read(a: int, n: int) -> int:
    q = ptr32(a)
    s = 0
    i = 0
    while i < n:
        s += q[0]
        i += 1
    return s
@micropython.viper
def write(a: int, n: int):
    q = ptr32(a)
    i = 0
    while i < n:
        q[0] = 0
        i += 1
@micropython.viper
def plain(n: int) -> int:
    s = 0
    i = 0
    while i < n:
        s += i
        i += 1
    return s
N = 100000
def ns(fn, *args):
    best = None
    for _ in range(3):
        t0 = time.ticks_us(); fn(*args, N); t = time.ticks_diff(time.ticks_us(), t0)
        best = t if best is None or t < best else best
    return best * 1000 // N
base = ns(plain)
"""


def test_reading_a_register_costs_what_it_does_on_a_watch(python):
    extra = measure(python, REGISTER_LOOPS + """
print({name: ns(read, a) - base for name, a in (
    ("memory", 0x3FC90000), ("gpio", 0x60004004), ("systimer", 0x60023044),
    ("system", 0x600C0018), ("rtc", 0x60008000), ("efuse", 0x60007044))})
""")
    # Nanoseconds over an empty loop, at 160 MHz. The RTC's registers are slow to reach.
    expected = {"memory": 75, "gpio": 156, "systimer": 156, "system": 131, "rtc": 452, "efuse": 452}
    for name, ns in expected.items():
        assert abs(extra[name] - ns) <= 20, (name, extra)


def test_writing_a_register_costs_what_it_does_on_a_watch(python):
    extra = measure(python, REGISTER_LOOPS + """
print({name: ns(write, a) - base for name, a in (("gpio", 0x60004008), ("rtc", 0x60008050))})
""")
    assert abs(extra["gpio"] - 25) <= 15, extra
    assert abs(extra["rtc"] - 204) <= 25, extra


def test_code_is_no_slower_the_first_time_it_runs(python):
    # A NeoPixel write goes through a driver nothing else has used yet.
    first, later = measure(python, """
import time, machine
from machine import Pin
p = Pin(8, Pin.OUT)
def us():
    t0 = time.ticks_us(); machine.bitstream(p, 0, (400, 850, 800, 450), b"\\x00"); return time.ticks_diff(time.ticks_us(), t0)
first = us()
print((first, min(us() for _ in range(5))))
""")
    assert 100 <= later <= 400      # 0.2 ms on a watch
    assert first <= 1000            # 0.5 ms on a watch


# ---------------------------------------------------------------------------------------
# Time

def test_sleeps_end_when_they_do_on_a_watch(python):
    means = measure(python, """
import time
def mean(fn, arg, n):
    total = 0
    for _ in range(n):
        t0 = time.ticks_us(); fn(arg); total += time.ticks_diff(time.ticks_us(), t0)
    return total // n
print((mean(time.sleep_us, 300, 20), mean(time.sleep_ms, 5, 20), mean(time.sleep_ms, 55, 5)))
""")
    # A watch overshoots by 13 to 16 us.
    for took, asked in zip(means, (300, 5000, 55000)):
        assert asked <= took <= asked + 120, means


def test_hardware_timers_keep_their_period(python):
    counts = measure(python, """
import time
from machine import Timer
n = [0]
def tick(t):
    n[0] += 1
out = []
tm = Timer(0)
for period in (10, 1):
    n[0] = 0
    tm.init(period=period, mode=Timer.PERIODIC, callback=tick)
    time.sleep_ms(1000 if period == 10 else 500)
    tm.deinit()
    out.append(n[0])
print(out)
""")
    # 100 and 499 on a watch; a period that ran even 0.3% long would lose a second count.
    assert counts[0] in (99, 100)
    assert counts[1] in (498, 499, 500)


# ---------------------------------------------------------------------------------------
# Peripheral clocks and resets

GATING = """
from machine import mem32
EN0, RST0, EN1, RST1 = 0x600C0018, 0x600C0020, 0x600C001C, 0x600C0024
def rd(a):
    return "%08x" % (mem32[a] & 0xFFFFFFFF)
def trial(bit, addr, val, en=EN0, rst=RST0):
    out = []
    e0, r0 = mem32[en], mem32[rst]
    m = 1 << bit
    mem32[en] = e0 & ~m; mem32[rst] = r0 | m
    out.append(rd(addr))                                  # no clock, in reset
    mem32[addr] = val; out.append(rd(addr))               # written like that
    mem32[en] = e0 | m; out.append(rd(addr))              # clock on, still in reset
    mem32[addr] = val; out.append(rd(addr))               # written while in reset
    mem32[rst] = r0 & ~m; out.append(rd(addr))            # out of reset
    mem32[addr] = val; out.append(rd(addr))               # an ordinary write
    mem32[en] = e0 & ~m; out.append(rd(addr))             # clock off again
    mem32[addr] = val ^ 0xF; out.append(rd(addr))         # written without a clock
    mem32[en] = e0 | m; out.append(rd(addr))              # clock back on
    mem32[rst] = r0 | m; out.append(rd(addr))             # into reset
    mem32[rst] = r0 & ~m; out.append(rd(addr))            # and out
    mem32[en] = e0; mem32[rst] = r0
    out.append(rd(addr))                                  # as found
    return " ".join(out)
"""

# Each column is a step of `trial`, as read from a watch.
GATED = {
    "rmt": ("trial(9, 0x600160a0, 0x55)",
            "00000000 00000000 00000000 00000000 00000080 00000055 00000055 00000055 00000055 00000000 00000080 00000000"),
    "spi3": ("trial(16, 0x6002500c, 0x1001)",
             "00000000 00000000 00000000 00000000 80003043 00001001 00001001 00001001 00001001 00000000 80003043 00000000"),
    "uart1": ("trial(5, 0x60010014, 0x123)",
              "00000000 00000000 00000000 00000000 000002b6 00000123 00000123 00000123 00000123 00000000 000002b6 00000000"),
    "ledc": ("trial(11, 0x600190a0, 0x80000a)",
             "00800000 00800000 00800000 00800000 00800000 0080000a 0080000a 0080000a 0080000a 00800000 00800000 00800000"),
    "i2c1": ("trial(18, 0x60027000, 0x55)",
             "00000000 00000000 00000000 00000000 00000000 00000055 00000055 00000055 00000055 00000000 00000000 00000000"),
    "pcnt": ("trial(10, 0x60017000, 0x3c55)",
             "00003c10 00003c10 00003c10 00003c10 00003c10 00003c55 00003c55 00003c55 00003c55 00003c10 00003c10 00003c10"),
    "i2s0": ("trial(4, 0x6000f020, 0x55)",
             "00000000 00000000 00000000 00000000 00009600 00000004 00000004 00000004 00000004 00000000 00009600 00000000"),
    # The converter's registers take writes without its clock.
    "saradc": ("trial(28, 0x60040004, 0xa155)",
               "0000a1fe 0000a1fe 0000a1fe 0000a1fe 0000a1fe 0000a155 0000a155 0000a15a 0000a15a 0000a1fe 0000a1fe 0000a1fe"),
    "gdma": ("trial(6, 0x6003f044, 0x5, EN1, RST1)",
             "00000000 00000000 00000000 00000000 00000000 00000005 00000005 00000005 00000005 00000000 00000000 00000000"),
    "lcd_cam": ("trial(8, 0x60041000, 0x855, EN1, RST1)",
                "00000000 00000000 00000000 00000000 00000843 00000855 00000855 00000855 00000855 00000000 00000843 00000000"),
    "uhci0": ("trial(8, 0x60014000, 0x55)",
              "00000000 00000000 00000000 00000000 000006e0 00000055 00000055 00000055 00000055 00000000 000006e0 00000000"),
}


def test_a_peripheral_without_its_clock_or_in_reset_ignores_writes(python):
    calls = ", ".join(f"{name!r}: {call}" for name, (call, _) in GATED.items())
    seen = measure(python, GATING + f"print({{{calls}}})")
    assert seen == {name: columns for name, (_, columns) in GATED.items()}


# ---------------------------------------------------------------------------------------
# Registers

REGISTER_BLOCKS = {
    "SYSTEM": (0x600C0000, 0x00, 0xA4),
    "RTC_CNTL": (0x60008000, 0x00, 0x200),
    "RTC_IO": (0x60008400, 0x00, 0x100),
    "SENS": (0x60008800, 0x00, 0x160),
    "GPIO": (0x60004000, 0x00, 0x74),
    "GPIO_PIN": (0x60004000, 0x74, 0x150),
    "GPIO_IN_SEL": (0x60004000, 0x154, 0x554),
    "GPIO_OUT_SEL": (0x60004000, 0x554, 0x630),
    "IO_MUX": (0x60009000, 0x00, 0xC8),
    "TIMG0": (0x6001F000, 0x00, 0x100),
    "TIMG1": (0x60020000, 0x00, 0x100),
    "SYSTIMER": (0x60023000, 0x00, 0x80),
    "LEDC": (0x60019000, 0x00, 0x100),
    "SPI0": (0x60003000, 0x00, 0x100),
    "SPI1": (0x60002000, 0x00, 0x100),
    "SPI2": (0x60024000, 0x00, 0x100),
    "SPI3": (0x60025000, 0x00, 0x100),
    "I2C0": (0x60013000, 0x00, 0x100),
    "I2C1": (0x60027000, 0x00, 0x100),
    "UART0": (0x60000000, 0x04, 0x90),
    "UART1": (0x60010000, 0x04, 0x90),
    "USJ": (0x60038000, 0x04, 0x90),
    "SARADC": (0x60040000, 0x00, 0x80),
    "GDMA": (0x6003F000, 0x00, 0x80),
    "SYSCON": (0x60026000, 0x00, 0xC0),
    "RMT": (0x60016000, 0x20, 0x100),
    "PCNT": (0x60017000, 0x00, 0x70),
    "I2S0": (0x6000F000, 0x0C, 0x70),
    "INTMTX": (0x600C2000, 0x00, 0x1A0),
    "EXTMEM": (0x600C4000, 0x00, 0x180),
    "SENSITIVE": (0x600C1000, 0x00, 0x200),
    "ASSIST": (0x600CE000, 0x00, 0x80),
    "WORLD": (0x600D0000, 0x00, 0x20),
    "USB_WRAP": (0x60039000, 0x00, 0x10),
    "LCD_CAM": (0x60041000, 0x00, 0x40),
}

REGISTER_DUMP = """
from machine import mem32
for name, base, start, end in BLOCKS:
    for off in range(start, end, 4):
        print("%s+%03x %08x" % (name, off, mem32[base + off] & 0xFFFFFFFF))
"""

WATCH_REGISTERS = Path(__file__).with_name("watch_registers.txt")

# Registers left out of the comparison: those that count or measure, and so don't read the
# same twice on the watch itself, and those that depend on what kind of reset the chip last
# came out of. A watch on USB can't be power cycled from here, and the emulator starts from
# power-on.
VOLATILE_REGISTERS = {
    "ASSIST": set(range(0x050, 0x070, 4)),          # where each core was when it was reset
    "EXTMEM": {0x0D4},
    "I2C0": {0x01C},
    "I2C1": {0x01C},
    "RTC_CNTL": {0x010, 0x014, 0x054,
                 0x004, 0x01C, 0x038, 0x03C, 0x058, 0x05C, 0x060, 0x068, 0x074, 0x080, 0x088,
                 0x08C, 0x090, 0x0A0, 0x0B4, 0x0C4, 0x0CC, 0x0D0, 0x0D4, 0x108, 0x10C, 0x110,
                 0x130, 0x140, 0x14C, 0x150},
    "RTC_IO": {0x024},
    "SENS": set(range(0x0A0, 0x0E0, 4)) | {0x09C, 0x0E8},
    "SYSTIMER": {0x040, 0x044, 0x048, 0x04C, 0x070, 0x074, 0x078, 0x07C},
    "TIMG0": {0x004, 0x008, 0x068, 0x06C},
    "TIMG1": {0x004, 0x008, 0x068, 0x06C},
    "USJ": {0x038},                                 # follows the host's USB traffic
}

# Registers that read differently here than on the watch: the flash controller's, which
# hold traces of the last thing it did with the flash chip.
KNOWN_REGISTER_DIFFERENCES = {
    "SPI1": {0x010, 0x018, 0x02C, 0x038, 0x058, 0x05C, 0x060, 0x098, 0x0A0, 0x0F8},
}


def read_registers(python):
    blocks = [(name, *spec) for name, spec in REGISTER_BLOCKS.items()]
    output = python.run(f"BLOCKS = {blocks!r}" + REGISTER_DUMP, timeout=120)
    return dict(line.split() for line in output.splitlines() if "+" in line)


def test_registers_read_as_they_do_on_a_watch(python):
    """After MicroPython has started and before it has been asked for anything."""
    expected = dict(line.split() for line in WATCH_REGISTERS.read_text().splitlines()
                    if line and not line.startswith("#"))
    seen = read_registers(python)
    wrong = {name for name, value in expected.items() if seen.get(name) != value}
    known = {f"{block}+{offset:03x}" for block, offsets in KNOWN_REGISTER_DIFFERENCES.items()
             for offset in offsets}
    assert wrong - known == set(), sorted(
        f"{name} is {seen.get(name)}, {expected[name]} on a watch" for name in wrong - known)
    if not ON_WATCH:
        assert known - wrong == set(), "these now match, so they can come off the list"


# ---------------------------------------------------------------------------------------
# Parts the watch has no use for, which still have to behave

def test_neopixel_data_takes_the_time_it_takes_on_the_wire(python):
    took = measure(python, """
import time, machine
from machine import Pin
p = Pin(8, Pin.OUT)
out = {}
for n in (12, 48, 180):
    buf = bytes(n)
    times = []
    for _ in range(5):
        t0 = time.ticks_us(); machine.bitstream(p, 0, (400, 850, 800, 450), buf); times.append(time.ticks_diff(time.ticks_us(), t0))
    out[n] = sorted(times)[2]
print(out)
""")
    # 1.25 us a bit, 10 us a byte, on top of 0.2 ms to set the transmitter up.
    for count, expected_us in ((12, 300), (48, 700), (180, 2000)):
        assert expected_us - 150 <= took[count] <= expected_us + 350, took


def test_a_neopixel_strip_can_be_written(python):
    assert python.run("""
import neopixel
from machine import Pin
strip = neopixel.NeoPixel(Pin(8), 60)
strip[0] = (10, 20, 30)
strip.write()
print("written")
""").strip() == "written"


def test_rmt_sends_pulses_for_as_long_as_they_last(python):
    early, done, ms = measure(python, """
import time, esp32
from machine import Pin
r = esp32.RMT(0, pin=Pin(8), clock_div=80)
t0 = time.ticks_us()
r.write_pulses(1000, [1, 0] * 100)      # 200 pulses of 1 ms: several refills of its memory
early = r.wait_done(timeout=50)
done = r.wait_done(timeout=1000)
print((early, done, time.ticks_diff(time.ticks_us(), t0) // 1000))
r.deinit()
""")
    assert (early, done) == (False, True)
    assert 198 <= ms <= 215


I2S = """
import time
from machine import Pin, I2S, mem32
def regs(*offsets):
    return ["%08x" % (mem32[0x6000F000 + o] & 0xFFFFFFFF) for o in offsets]
def ms(fn, buf):
    t0 = time.ticks_us(); n = fn(buf); return n, time.ticks_diff(time.ticks_us(), t0) // 1000
"""


@pytest.mark.parametrize("settings, count, clocks", [
    # Bits per sample, format, rate, bytes in 100 ms; then the transmitter's width,
    # clock dividers and slots as the driver leaves them.
    ("16, I2S.MONO, 16000", 3200, ["6f3de38f", "34000027", "003c0001", "00010001"]),
    ("16, I2S.STEREO, 44100", 17640, ["6f3de38f", "3400000e", "00107a4c", "00010003"]),
    ("32, I2S.MONO, 22050", 8820, ["7f7fe19f", "3400001c", "00051298", "00010001"]),
])
def test_i2s_plays_at_the_sample_rate(python, settings, count, clocks):
    first, second, seen, state = measure(python, I2S + f"""
bits, fmt, rate = {settings}
a = I2S(0, sck=Pin(8), ws=Pin(9), sd=Pin(10), mode=I2S.TX, bits=bits, format=fmt, rate=rate, ibuf=20000)
buf = bytes({count})
first = ms(a.write, buf)
second = ms(a.write, buf)
print((first, second, regs(0x2c, 0x34, 0x3c, 0x54), regs(0x6c)))
a.deinit()
""")
    assert seen == clocks
    assert state == ["00000000"]
    assert first[0] == second[0] == count
    # The first write also waits for the buffers ahead of it to drain.
    assert 92 <= second[1] <= 106 and 98 <= first[1] <= 135, (first, second)
    assert 198 <= first[1] + second[1] <= 240, (first, second)


def test_i2s_records_silence_at_the_sample_rate(python):
    first, second, silent, seen = measure(python, I2S + """
a = I2S(0, sck=Pin(8), ws=Pin(9), sd=Pin(10), mode=I2S.RX, bits=16, format=I2S.MONO, rate=16000, ibuf=20000)
buf = bytearray(3200)
first = ms(a.readinto, buf)
second = ms(a.readinto, buf)
print((first, second, not any(buf), regs(0x20, 0x28, 0x30, 0x50, 0x64, 0x6c)))
a.deinit()
""")
    assert silent
    # Byte order is a setting the driver leaves to whatever was in memory.
    seen[0] = "%08x" % (int(seen[0], 16) & ~0x80)
    assert seen == ["00081404", "3f7fe19f", "34000027", "00010003", "00000800", "00000001"]
    assert first[0] == second[0] == 3200
    assert 92 <= second[1] <= 106 and 98 <= first[1] <= 135, (first, second)


def test_touch_pads_read_like_bare_pads(python):
    counts = measure(python, """
from machine import Pin, TouchPad
print([TouchPad(Pin(n)).read() for n in (8, 9, 10, 11)])
""")
    # 11188 to 12429 across the pads of a watch, from one day to the next.
    assert all(10500 <= count <= 13000 for count in counts), counts


def test_uart_and_spi_lines_tied_to_themselves(python):
    uart, spi, spi_ms = measure(python, """
import time
from machine import Pin, UART, SPI
u = UART(1, baudrate=115200, tx=9, rx=9, timeout=50)
u.write(b"loopback-1234567890")
time.sleep_ms(30)
uart = u.read()
u.deinit()
s = SPI(2, baudrate=100000, sck=Pin(8), mosi=Pin(10), miso=Pin(11))
buf = bytearray(8)
s.write_readinto(b"\\x01\\x02\\x7f\\x80\\xaa\\x55\\xff\\x00", buf)
t0 = time.ticks_us(); s.write(bytes(125)); spi_ms = time.ticks_diff(time.ticks_us(), t0) / 1000
s.deinit()
print((uart, bytes(buf), spi_ms))
""")
    # One pad can't be both ends of a UART, and an undriven MISO reads low.
    assert uart is None
    assert spi == bytes(8)
    assert 10 <= spi_ms <= 12       # a thousand bits at 100 kHz


@pytest.mark.parametrize("baud, count, dividers", [
    # The fractional divider and the source clock's, as the driver sets them for each rate.
    (1200, 16, [0x00B00E77, 0x03708000]),
    (9600, 200, [0x00500823, 0x03701000]),
    (115200, 200, [0x0030015B, 0x03700000]),
    (921600, 1000, [0x0060002B, 0x03700000]),
])
def test_uart_bytes_take_their_time_on_the_wire(python, baud, count, dividers):
    write_us, drain_us, seen = measure(python, f"""
import time
from machine import UART, Pin, mem32
u = UART(1, baudrate={baud}, tx=9, rx=10)
buf = bytes({count})
t0 = time.ticks_us(); u.write(buf); t1 = time.ticks_us(); u.flush(); t2 = time.ticks_us()
print((time.ticks_diff(t1, t0), time.ticks_diff(t2, t1),
       [mem32[0x60010014] & 0xFFFFFFFF, mem32[0x60010078] & 0xFFFFFFFF]))
u.deinit()
""")
    assert seen == dividers
    # Ten bits a byte: start, eight of data, stop.
    expected_ms = count * 10 * 1000 / baud
    assert abs((write_us + drain_us) / 1000 - expected_ms) <= max(0.4, expected_ms * 0.03)


# ---------------------------------------------------------------------------------------
# The watch's own parts

def test_the_library_starts_as_fast_as_on_a_watch(python):
    import_ms, begin_ms, commit_us = measure(python, """
import time
t0 = time.ticks_us()
from Artemis import *
t1 = time.ticks_us()
begin()
t2 = time.ticks_us()
display.fill(display.Color.Red)
t3 = time.ticks_us(); display.commit(); t4 = time.ticks_us()
backlight.off()
print((time.ticks_diff(t1, t0) // 1000, time.ticks_diff(t2, t1) // 1000, time.ticks_diff(t4, t3)))
""")
    # Compiling the library's source leans on memory and flash, which are faster here.
    assert within(import_ms, 160, 0.17)
    assert within(begin_ms, 619, 0.05)
    assert within(commit_us, 18236, 0.08)


def test_battery_sense_on_usb_and_its_reference(python):
    battery_mv, reference_mv = measure(python, """
import time
from machine import Pin, ADC
calib = Pin(4, Pin.OUT, value=0)
a = ADC(Pin(5), atten=ADC.ATTN_11DB)
def mv():
    time.sleep_ms(20)
    return sorted(a.read_uv() for _ in range(15))[7] // 1000
battery = mv()
calib.value(1)
reference = mv()
calib.value(0)
print((battery, reference))
""")
    # On USB the divider sees the 5 V rail through a diode rather than the battery.
    assert 1070 <= battery_mv <= 1150
    assert 605 <= reference_mv <= 635


def test_pads_and_pulls(python):
    pins, pulled = measure(python, """
import time
from machine import Pin, ADC
pins = [Pin(p, Pin.IN).value() for p in (1, 2, 3, 21, 42, 6, 7, 38, 39)]
a = ADC(Pin(8), atten=ADC.ATTN_11DB)
Pin(8, Pin.IN, Pin.PULL_UP); time.sleep_ms(2); up = a.read_uv() // 1000
Pin(8, Pin.IN, Pin.PULL_DOWN); time.sleep_ms(2); down = a.read_uv() // 1000
Pin(8, Pin.IN, None)
print((pins, (up, down)))
""")
    # Lever and button idle low, USB is plugged in, the charger nets are high, and the
    # motion sensor's interrupt lines are quiet.
    assert pins == [0, 0, 0, 0, 1, 1, 1, 0, 0]
    # A pad reads 0.34 V against its pull-down once its touch sensor has been used.
    assert 2700 <= pulled[0] <= 3200 and pulled[1] <= 420, pulled


MOTION_SENSOR = """
import time
from machine import I2C, Pin
i2c = I2C(0, sda=Pin(40), scl=Pin(41), freq=400000)
def rd(reg, n=1):
    return i2c.readfrom_mem(0x6A, reg, n)
def wr(reg, v):
    i2c.writeto_mem(0x6A, reg, bytes([v]))
def s16(b, i=0):
    v = b[i] | b[i + 1] << 8
    return v - 65536 if v > 32767 else v
def fifo():
    st = rd(0x3A, 4)
    return st[0] | (st[1] & 7) << 8, st[1]
wr(0x12, 0x01); time.sleep_ms(30)          # SW_RESET
wr(0x12, 0x44)                             # BDU, IF_INC
"""


def test_motion_sensor_samples_at_its_rates(python):
    first_ms, rates, norm = measure(python, MOTION_SENSOR + """
def rate():
    n = 0
    t0 = time.ticks_us()
    while time.ticks_diff(time.ticks_us(), t0) < 1000000:
        if rd(0x1E)[0] & 1:
            rd(0x28, 6); n += 1
    return n
wr(0x10, 0x40)                             # accelerometer at 104 Hz
t0 = time.ticks_us()
rd(0x28, 6)
while not rd(0x1E)[0] & 1:
    pass
first = time.ticks_diff(time.ticks_us(), t0) // 1000
rates = [rate()]
for odr in (0x30, 0x60):
    wr(0x10, odr); time.sleep_ms(50); rates.append(rate())
wr(0x10, 0x40); time.sleep_ms(40)
g = [s16(rd(0x28, 6), i) for i in (0, 2, 4)]
print((first, rates, round(sum(v * v for v in g) ** 0.5 * 0.061)))
wr(0x12, 0x01)
""", timeout=90)
    assert 7 <= first_ms <= 12
    # This watch's sensor runs 4.5% fast, which is within its tolerance: 109, 55 and 435.
    for rate, nominal in zip(rates, (104, 52, 416)):
        assert nominal * 0.97 <= rate <= nominal * 1.07, rates
    assert 1040 <= norm <= 1070             # mg, lying still


def test_motion_sensor_fifo_and_interrupts(python):
    seen = measure(python, MOTION_SENSOR + """
out = {}
wr(0x10, 0x40); wr(0x11, 0x40)             # both sensors at 104 Hz
time.sleep_ms(100)
wr(0x06, 0x00); wr(0x07, 0x00)
wr(0x08, 0x09)                             # both into the FIFO
wr(0x0A, 0x00); wr(0x0A, 0x26)             # continuous at 104 Hz
time.sleep_ms(500)
out["after 500 ms"] = fifo()
wr(0x0A, 0x00); wr(0x0A, 0x56)
wr(0x10, 0x70); wr(0x11, 0x70)             # 833 Hz: let it overflow
time.sleep_ms(1200)
out["overflowed"] = fifo()
wr(0x0A, 0x00)
out["bypassed"] = fifo()
wr(0x10, 0x40); wr(0x11, 0x40)
wr(0x06, 60); wr(0x07, 0x00)               # watermark at 60 words
wr(0x0A, 0x21)                             # FIFO mode stops when full
time.sleep_ms(200)
out["fifo mode"] = fifo()
wr(0x0D, 0x08)                             # watermark on INT1
time.sleep_ms(5)
out["int1 on watermark"] = Pin(38, Pin.IN).value()
wr(0x0D, 0x00); wr(0x0A, 0x00)
out["int1 off"] = Pin(38).value()
def data_ready(pin, reg):
    # High once a sample is waiting, low again once it has been read. Now and then the
    # next sample lands before the pin can be looked at.
    time.sleep_ms(30)
    before = pin.value()
    lows = 0
    for _ in range(8):
        rd(reg, 6); lows += not pin.value()
        time.sleep_ms(12)
    return before, lows >= 6
wr(0x0D, 0x01)
out["int1 data ready"] = data_ready(Pin(38), 0x28)
wr(0x0D, 0x00)
wr(0x0E, 0x02)
out["int2 data ready"] = data_ready(Pin(39, Pin.IN), 0x22)
wr(0x0E, 0x00)
def stamps():
    time.sleep_ms(5)
    a = rd(0x40, 3); time.sleep_ms(100); b = rd(0x40, 3)
    return (b[0] | b[1] << 8 | b[2] << 16) - (a[0] | a[1] << 8 | a[2] << 16)
wr(0x19, 0x20)                             # timestamps, 6.4 ms apart
out["timestamps"] = stamps()
wr(0x5C, 0x10)                             # then 25 us apart
out["fine timestamps"] = stamps()
print(out)
wr(0x12, 0x01)
""", timeout=90)
    words, status = seen.pop("after 500 ms")
    assert 300 <= words <= 336 and status == 0x01          # 318 on a watch
    words, status = seen.pop("fifo mode")
    assert 114 <= words <= 132 and status == 0x80          # 126 on a watch
    assert 15 <= seen.pop("timestamps") <= 17
    assert 3900 <= seen.pop("fine timestamps") <= 4250     # 4152 on a watch
    assert seen == {
        "overflowed": (0, 0x60),
        "bypassed": (0, 0x10),
        "int1 on watermark": 1,
        "int1 off": 0,
        "int1 data ready": (1, True),
        "int2 data ready": (1, True),
    }


def test_pwm_changes_take_effect_at_the_end_of_a_period(python):
    seen = measure(python, """
import time
from machine import Pin, PWM, mem32
def level():
    return mem32[0x6000403C] >> 8 & 1
def watch_pad(ms):
    high = n = edges = 0
    last = level()
    t0 = time.ticks_us()
    while time.ticks_diff(time.ticks_us(), t0) < ms * 1000:
        v = level()
        high += v; n += 1
        edges += v != last
        last = v
    return round(high / n, 2), round(edges * 1000 / ms, -2)
out = {}
p = PWM(Pin(8), freq=200, duty_u16=16384)
mem32[0x60009004 + 4 * 8] |= 1 << 9        # the pad's input, to watch it through
time.sleep_ms(20)
out["a quarter"] = watch_pad(500)
p.duty_u16(49152)
out["read back at once"] = p.duty_u16()
time.sleep_ms(12)
out["three quarters"] = watch_pad(500)
p.duty_u16(0); time.sleep_ms(12)
out["off"] = watch_pad(100)
p.duty_u16(65535); time.sleep_ms(12)
out["full"] = watch_pad(100)
p.deinit()
out["after deinit"] = watch_pad(100)
Pin(8, Pin.IN, None)
print(out)
""")
    assert seen == {
        "a quarter": (0.25, 400.0),
        "read back at once": 16384,
        "three quarters": (0.75, 400.0),
        "off": (0.0, 0.0),
        "full": (1.0, 0.0),
        "after deinit": (0.0, 0.0),
    }


# ---------------------------------------------------------------------------------------
# Resets

RTC_WATCHDOG = """
import time
from machine import mem32
R = 0x60008000
def wdt(stage0, hold0, stage1=0, hold1=0):
    mem32[R + 0xB0] = 0x50D83AA1            # the key that lets its registers be written
    mem32[R + 0xAC] = 0x80000000            # feed
    mem32[R + 0x9C] = hold0
    mem32[R + 0xA0] = hold1
    mem32[R + 0x98] = 1 << 31 | stage0 << 28 | stage1 << 25 | 7 << 16 | 7 << 13 | 0xE00
    mem32[R + 0xB0] = 0
def feed():
    mem32[R + 0xB0] = 0x50D83AA1
    mem32[R + 0xAC] = 0x80000000
    mem32[R + 0xB0] = 0
def run_until_reset():
    # Keeps the time in a register that a reset of the digital side leaves alone.
    t0 = time.ticks_ms()
    while True:
        mem32[R + 0x50] = time.ticks_diff(time.ticks_ms(), t0)
"""

# What MicroPython calls the reset, what the chip does (for each core, in six bits), the
# register above, and the system clock.
AFTER_RESET = """
import time, machine
from machine import mem32
print((machine.reset_cause(), mem32[0x60008038] & 0xFFF, mem32[0x60008050], time.time()))
"""


def reset_by(python, code):
    """Runs code that ends in a reset, and returns a session on the watch once it is back."""
    import time

    target = python.watch
    before = len(target.usb_output)
    with pytest.raises((WatchRestarted, TimeoutError, OSError)):
        python.run(code, timeout=8)
    if ON_WATCH and not target.running:
        # A whole-chip reset takes it off USB, and what it prints meanwhile is lost.
        target.reconnect(timeout=20)
        time.sleep(1.5)
    else:
        deadline = time.monotonic() + 15
        while "MicroPython v" not in target.usb_output[before:]:
            assert time.monotonic() < deadline, "the watch didn't come back"
            time.sleep(0.02)
    return PythonSession(target, boot_timeout=30)


@pytest.mark.parametrize("arm, state, elapsed_ms", [
    ("wdt(3, 15000)", 0x249, 200),              # the digital system
    ("wdt(3, 60000)", 0x249, 798),
    ("wdt(2, 15000)", 0x34D, 200),              # the two CPUs
    ("wdt(0, 15000, 3, 30000)", 0x249, 200),    # a stage that does nothing takes no time
    ("wdt(1, 15000, 3, 30000)", 0x249, 400),    # an interrupt first, then the reset
])
def test_the_rtc_watchdog_resets_when_its_time_is_up(python, arm, state, elapsed_ms):
    session = reset_by(python, RTC_WATCHDOG + arm + "\nrun_until_reset()")
    cause, seen, ms, _ = measure(session, AFTER_RESET)
    assert (cause, seen) == (3, state)
    # Stage 0 lasts twice its register's count of a slow clock that runs at 150 kHz here.
    assert abs(ms - elapsed_ms) <= max(6, elapsed_ms * 0.02)


def test_feeding_the_rtc_watchdog_holds_it_off(python):
    session = reset_by(python, RTC_WATCHDOG + """
wdt(3, 30000)
for _ in range(10):
    time.sleep_ms(100); feed()
run_until_reset()
""")
    cause, state, ms, _ = measure(session, AFTER_RESET)
    assert (cause, state) == (3, 0x249)
    assert abs(ms - 399) <= 10


def test_the_rtc_watchdog_is_locked_without_its_key(python):
    seen = measure(python, RTC_WATCHDOG + """
wdt(0, 15000)
mem32[R + 0x98] = 1 << 31 | 3 << 28
mem32[R + 0x9C] = 77
mem32[R + 0xAC] = 0x80000000
locked = [mem32[R + o] & 0xFFFFFFFF for o in (0x98, 0x9C, 0xAC, 0xB0)]
mem32[R + 0xB0] = 0x50D83AA1
key = mem32[R + 0xB0] & 0xFFFFFFFF
mem32[R + 0x98] = 0x7EE00
mem32[R + 0xB0] = 0
print((locked, key, mem32[R + 0x98] & 0xFFFFFFFF))
""")
    assert seen == ([0x8007EE00, 15000, 0, 0], 0x50D83AA1, 0x7EE00)


def test_the_rtc_watchdog_can_reset_the_whole_chip(python):
    python.run("from machine import mem32\nmem32[0x60008050] = 1234")
    session = reset_by(python, RTC_WATCHDOG + "wdt(4, 15000)\nwhile True: pass")
    cause, state, store, now = measure(session, AFTER_RESET)
    assert (cause, state, store) == (3, 0x410, 0)
    assert now <= 3                             # its clock starts again


def test_a_software_reset_leaves_the_rtc_domain_alone(python):
    before = measure(python, """
import time
from machine import mem32
mem32[0x60008050] = 0x1234ABCD
time.sleep(2)
print(time.time())
""")
    session = reset_by(python, "import machine\nmachine.reset()")
    cause, state, store, now = measure(session, AFTER_RESET)
    assert (cause, state, store) == (2, 0x30C, 0x1234ABCD)
    assert before <= now <= before + 3          # and the clock carries on


def test_the_bootloader_and_firmware_leave_the_rtc_watchdog_set_up_and_off(python):
    seen = measure(python, """
from machine import mem32
print([mem32[0x60008000 + o] & 0xFFFFFFFF for o in (0x98, 0x9C, 0xB0)])
""")
    # Armed for 9 s of boot by the bootloader, to reset the whole chip; turned off, and
    # locked, once the firmware is up.
    assert seen == [0x4007EE00, 0x000956A0, 0]


def record_watch_registers(resets=6):
    """Rewrites watch_registers.txt with what reads the same after each of several resets."""
    dumps = []
    for _ in range(resets):
        with Device(os.environ.get("ARTEMIS_WATCH_PORT") or None) as device:
            dumps.append(read_registers(fresh_watch(device)))
    volatile = {f"{block}+{offset:03x}" for block, offsets in VOLATILE_REGISTERS.items()
                for offset in offsets}
    stable = [name for name in dumps[0] if name not in volatile
              and all(dump.get(name) == dumps[0][name] for dump in dumps)]
    WATCH_REGISTERS.write_text(
        "# ESP32-S3 registers as a watch (hardware revision 2, on USB) has them once MicroPython\n"
        "# has started: those that read the same after every whole-chip reset. Written, with a watch\n"
        "# plugged in, by: python -m tests.test_like_a_watch\n"
        + "".join(f"{name} {dumps[0][name]}\n" for name in stable))
    print(f"{len(stable)} of {len(dumps[0])} registers are stable")


if __name__ == "__main__":
    record_watch_registers()

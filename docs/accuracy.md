# What's emulated, and how closely

## What's emulated

| Part | Status |
| --- | --- |
| ESP32-S3 | both cores, ROM, 4 MB flash, 2 MB PSRAM, at 20 to 240 MHz, running code at the chip's speed |
| Display | ST7735S over SPI with DMA, on both hardware revisions |
| Inputs and outputs | lever, side button, six LEDs, RGB LED, backlight and buzzer, including PWM and hardware fades |
| Motion sensor | LSM6DS3TR-C: accelerometer, gyroscope, FIFO, timestamps, interrupts; wrist tilt, tap, steps, free fall, wake-up and orientation |
| Clock | BM8563 real-time clock |
| Power | battery voltage through the ADC, USB and charging detection, the charger's status line, the power-off latch |
| Serial | USB Serial/JTAG console, UART0 |
| Sleep | light and deep sleep; wakeup by timer, EXT0, EXT1 and GPIO; RTC memory is kept |
| Watchdogs | task, interrupt and RTC watchdogs, with panics and restarts as on the chip |
| Storage | flash, NVS, and MicroPython's filesystem |
| Other parts of the chip | RMT (NeoPixels), I2S, touch pads, timers, the ADC, and UART, SPI and I2C ports with nothing on them: none is wired to anything on the watch, but code that uses them runs as it does there |
| Bluetooth | the real controller runs: advertising, scanning and GATT servers work and connection attempts time out, but nothing goes on the air |
| Wi-Fi | the stack starts in both modes; scans find nothing and connecting never succeeds. Turning it off takes five seconds, where a watch takes milliseconds |

## How close it is to a real watch

`tests/test_like_a_watch.py` holds what was measured on a watch (hardware revision 2, running this project's MicroPython) and requires the emulator to do the same. With a watch plugged in, `ARTEMIS_TEST_ON_WATCH=1` runs those very tests on it instead, which is how the figures in them were checked.

- **Speed.** Code takes the time it takes on the chip, at any CPU frequency. Of 27 MicroPython operations timed on both, 17 are within 3% and all within 11%; six longer workloads are within 3% to 8%. Work that leans on memory and flash, such as compiling Python source or building strings, is the furthest out, up to 15% fast. The first run of a piece of code is no slower than the next. Reading or writing a peripheral's register costs what it does on the chip, RTC registers more than the rest.
- **Time.** Sleeps, timers, PWM, UART and sensor data rates keep the chip's timing, and clocks that code can read advance with the instructions it runs. An idle core wakes for its tick within 5 microseconds of when it is due as a rule and 60 at worst, where a watch manages 1 and 23.
- **Registers.** Of 2004 registers that read the same on a watch from one reset to the next, 1994 read the same here once MicroPython has started. The other ten are the flash controller's, and are listed in the test. A peripheral without its clock, or held in reset, ignores writes as it does on the chip.
- **Resets.** Each kind of reset leaves what it leaves on a watch: a software or watchdog reset keeps the RTC domain, with its clock and scratch registers, and a whole-chip reset doesn't.

Keeping that timing costs about one core of your computer while the watch is busy, and a seventh of one while it idles. `--turbo` runs unpaced instead.

## What a test on the emulator can't tell you

- **Radio traffic.** There's nothing to pair with or connect to, so phone pairing can't be tested.
- **Cache misses.** Code and data in flash or PSRAM are as fast as when cached, which is why memory-heavy work runs a little fast.
- **Analogue behaviour.** The ADC reads ideal voltages through a real chip's calibration. LEDs and the backlight are shown by duty cycle, the buzzer as an ideal square wave, and nothing draws current.
- **Parts with no model.** The ULP coprocessor, USB OTG, TWAI, motor PWM, SD host, camera and LCD interfaces keep their register values and do nothing.
- **The exact instruction an interrupt lands on.** It can be a microsecond or two off, which only shows where firmware is sensitive to it: the interrupt watchdog's panic reads "Unhandled debug exception" one time in thirty here, and never on the watch.
- **Timing in a virtual machine.** The figures above are for Linux on the computer itself. In a virtual machine, which is what WSL is, the emulator's threads cost more to coordinate, and code that works a peripheral hard falls behind: in an Ubuntu VM, 42 or 43 of the 45 comparisons hold, and sending a frame to the display, driving NeoPixels and a 921600 baud UART measure up to a third slow by the watch's own clock.
- **Other watches.** One watch was measured. Another's oscillators, sensor offsets and ADC will differ a little; the motion sensor of the one measured runs 4.5% fast, where the emulator's is exact.

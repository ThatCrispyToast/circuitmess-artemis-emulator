# Emulation accuracy

## Emulated hardware

| Part | Status |
| --- | --- |
| ESP32-S3 | Both cores, ROM, 4 MB flash, 2 MB PSRAM, CPU frequencies from 20 to 240 MHz |
| Display | ST7735S over SPI with DMA, on both hardware revisions |
| Inputs and outputs | Lever, side button, six LEDs, RGB LED, backlight, and buzzer, including PWM and hardware fades |
| Motion sensor | LSM6DS3TR-C accelerometer, gyroscope, FIFO, timestamps, and interrupts; wrist tilt, tap, steps, free fall, wake-up, and orientation |
| Clock | BM8563 real-time clock |
| Power | Battery voltage through the ADC, USB and charging detection, charger status, and power-off latch |
| Serial | USB Serial/JTAG console and UART0 |
| Sleep | Light and deep sleep; timer, EXT0, EXT1, and GPIO wake-up; RTC memory retention |
| Watchdogs | Task, interrupt, and RTC watchdogs, including panics and restarts |
| Storage | Flash, NVS, and MicroPython's filesystem |
| Other peripherals | RMT (NeoPixels), I2S, touch pads, timers, ADC, and unused UART, SPI, and I2C ports |
| Bluetooth | Controller firmware runs advertising, scanning, and GATT servers; no radio transmission; connection attempts time out |
| Wi-Fi | Station and access-point modes start; scans return no results and connections fail. Shutdown takes five seconds, versus milliseconds on hardware |

## Hardware comparisons

Measurements compare the emulator on native Linux with one hardware revision 2 watch running this project's MicroPython. [`tests/test_hardware_comparison.py`](../tests/test_hardware_comparison.py) contains hardware baselines and tests that run on either target. Set `ARTEMIS_TEST_ON_WATCH=1` to run them on a connected watch; see the [testing guide](testing.md#on-a-real-watch).

| Metric | Recorded result |
| --- | --- |
| MicroPython execution time | 17 of 27 operations within 3% of hardware; all within 11% |
| Longer workloads | Six workloads differ by 3–8% |
| Memory-heavy workloads | Up to 15% faster, including source compilation and string construction |
| Idle-core tick wake-up delay | Emulator: typically ≤5 µs, worst observed 60 µs. Hardware: typically ≤1 µs, worst observed 23 µs |
| Registers after MicroPython startup | 1994 of 2004 stable register values match (99.5%); the 10 differences are in the flash controller |

The following table lists acceptance thresholds used by the hardware comparison tests.

| Test | Reference | Acceptance limit |
| --- | --- | --- |
| Sleep duration | Requests of 300 µs, 5 ms, and 55 ms; hardware overshoots by 13–16 µs | Mean duration from the requested time to 120 µs above it |
| Register read overhead at 160 MHz | GPIO: 156 ns; RTC: 452 ns, relative to an empty loop | Within 20 ns of each reference |
| UART transmission | 1,200, 9,600, 115,200, and 921,600 baud; 10 bits per byte | Within 3% of nominal transmission time or 0.4 ms, whichever is larger |
| RTC watchdog reset | Hardware baselines of 200, 400, and 798 ms | Within 2% or 6 ms, whichever is larger |

Firmware-visible clocks advance with instruction execution. Tests also cover hardware timer periods, PWM, sensor sample rates, register writes, and peripheral clock gating and reset behavior. Software and digital-system watchdog resets preserve the RTC clock and scratch registers; whole-chip resets clear them.

CPU pacing uses about one host core while the watch is busy and one seventh of a core while idle. `--turbo` disables pacing.

## Limitations

- Bluetooth and Wi-Fi cannot transmit or connect, so phone pairing cannot be tested.
- Flash and PSRAM accesses use cached timings, with no cache-miss penalty on the first execution of a code path.
- ADC inputs are ideal voltages processed through chip calibration. LED and backlight brightness follow PWM duty cycles, and buzzer audio is an ideal square wave. Power consumption is not modeled.
- The ULP coprocessor, USB OTG, TWAI, motor PWM, SD host, camera, and LCD interfaces store register values but have no functional model.
- Interrupt delivery can differ by 1–2 µs. The interrupt watchdog reported "Unhandled debug exception" in about 1 of 30 emulator runs and none of the hardware runs.
- Virtual machines, including WSL, add thread synchronization overhead. In an Ubuntu VM, 42–43 of 45 comparisons pass. Display updates, NeoPixel output, and UART at 921,600 baud run up to a third slower by the guest clock.
- Results are based on one watch. Oscillator frequencies, sensor offsets, and ADC readings vary between devices. The measured watch's motion sensor runs 4.5% above its nominal rate; the emulator uses the nominal rate.

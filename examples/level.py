"""A spirit level: the bubble follows the accelerometer, and the LEDs show the way to level."""

from Artemis import *
import time

begin()

SIZE = 128
BUBBLE = 6
RANGE = 48  # pixels of travel per g
LEVEL_WITHIN = 0.05

while True:
    buttons.scan()
    if buttons.state(Buttons.Back):
        break

    x, y, _ = imu.get_accel()
    bubble_x = SIZE // 2 - int(x * RANGE)
    bubble_y = SIZE // 2 + int(y * RANGE)
    is_level = abs(x) < LEVEL_WITHIN and abs(y) < LEVEL_WITHIN

    display.fill(display.Color.Black)
    display.hline(0, SIZE // 2, SIZE, display.Color.Gray)
    display.vline(SIZE // 2, 0, SIZE, display.Color.Gray)
    display.fill_rect(
        bubble_x - BUBBLE,
        bubble_y - BUBBLE,
        BUBBLE * 2,
        BUBBLE * 2,
        display.Color.Green if is_level else display.Color.Yellow,
    )
    display.commit()

    rgb.set(0, 100 if is_level else 0, 0)
    time.sleep_ms(30)

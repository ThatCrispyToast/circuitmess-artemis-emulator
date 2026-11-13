"""Shows the time from the RTC, with the seconds in binary on the LEDs."""

from Artemis import *
import time

begin()

while True:
    buttons.scan()
    if buttons.state(Buttons.Back):
        break

    now = rtc.get_time()
    display.fill(display.Color.Black)
    display.text("%02d:%02d:%02d" % (now.hours, now.minutes, now.seconds), 32, 50, display.Color.White)
    display.text("%04d-%02d-%02d" % (now.year, now.month, now.day), 24, 70, display.Color.Gray)
    display.commit()

    for i in range(len(leds)):
        leds[i].value(now.seconds >> i & 1)
    time.sleep_ms(200)

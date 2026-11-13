"""Greets you, counts lever pushes on the LEDs, and ends when the side button is pressed."""

from Artemis import *
import time

begin()

pushes = 0


def counted():
    global pushes
    pushes += 1


buttons.on_press(Buttons.Select, counted)

while True:
    buttons.scan()
    if buttons.state(Buttons.Back):
        break

    display.fill(display.Color.Navy)
    display.text("Hello,", 20, 30, display.Color.White)
    display.text("Artemis!", 20, 44, display.Color.Yellow)
    display.text("pushes: %d" % pushes, 20, 80, display.Color.Cyan)
    display.commit()

    for i in range(len(leds)):
        leds[i].value(i < pushes % (len(leds) + 1))
    rgb.set(0, 100 if buttons.state(Buttons.Select) else 0, 0)
    time.sleep_ms(30)

print("pushed", pushes, "times")

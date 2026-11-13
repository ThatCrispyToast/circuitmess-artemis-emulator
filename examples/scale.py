"""Plays a scale on the buzzer, walking along the LEDs, then finishes."""

from Artemis import *

begin()

NOTES = (523, 587, 659, 698, 784, 880)

for i, note in enumerate(NOTES):
    leds[i].on()
    piezo.tone(note, 150)
    leds[i].off()

print("done")

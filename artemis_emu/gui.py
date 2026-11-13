"""The watch on your desktop: display, lever, button, LEDs, buzzer and sensors."""

from __future__ import annotations

import array
import math
import os
import time
from dataclasses import dataclass
from typing import Callable, Optional

# Keeps the window title honest and pygame quiet on the terminal we share with firmware.
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

import pygame  # noqa: E402

from .image import save_png  # noqa: E402
from .machine import LCD_HEIGHT, LCD_WIDTH, Watch  # noqa: E402

BACKGROUND = (24, 26, 30)
PANEL = (34, 37, 43)
PANEL_EDGE = (58, 62, 70)
TEXT = (226, 228, 232)
TEXT_DIM = (150, 155, 165)
ACCENT = (255, 118, 20)
CASE = (196, 204, 210)
CASE_EDGE = (150, 158, 166)
PCB = (22, 24, 26)
STRAP = (16, 16, 18)
METAL = (176, 180, 184)

LED_ICE_BLUE = (120, 225, 255)
LED_ORANGE = (255, 150, 30)

PANEL_WIDTH = 300
MARGIN = 16
STRAP_HEIGHT = 56
PCB_WIDTH = 176


def _lit(color: tuple[int, int, int], amount: float) -> tuple[int, int, int]:
    off = 42
    return tuple(int(off + (c - off) * amount) for c in color)


class Buzzer:
    """Plays what's on the piezo's pin as a square wave."""

    RATE = 44100

    def __init__(self, enabled: bool = True):
        self.muted = not enabled
        self.available = False
        self._channel = None
        self._freq = 0
        self._cache: dict[int, pygame.mixer.Sound] = {}
        if enabled:
            try:
                pygame.mixer.init(self.RATE, -16, 1, 512)
                self.available = True
            except pygame.error:
                pass

    def _sound(self, freq: int) -> pygame.mixer.Sound:
        if freq not in self._cache:
            period = self.RATE / freq
            periods = max(1, round(2048 / period))
            samples = array.array("h")
            for i in range(int(periods * period)):
                samples.append(6000 if (i / period) % 1 < 0.5 else -6000)
            if len(self._cache) > 256:
                self._cache.clear()
            self._cache[freq] = pygame.mixer.Sound(buffer=samples.tobytes())
        return self._cache[freq]

    def set(self, freq: int, duty: float) -> None:
        if not self.available:
            return
        if self.muted or not 20 <= freq <= 20000 or duty <= 0:
            if self._channel:
                self._channel.stop()
            self._freq = 0
            return
        if freq != self._freq:
            if self._channel:
                self._channel.stop()
            self._channel = self._sound(freq).play(loops=-1)
            self._freq = freq


@dataclass
class _Button:
    label: str
    rect: pygame.Rect
    action: Callable[[], None]
    active: Callable[[], bool] = lambda: False


class WatchWindow:
    def __init__(
        self,
        watch: Watch,
        scale: int = 3,
        sound: bool = True,
        title: str = "Artemis Watch 2.0",
        screenshot_dir: str = ".",
    ):
        self.watch = watch
        self.scale = scale
        self.screenshot_dir = screenshot_dir
        self.title = title

        pygame.display.init()
        pygame.font.init()

        lcd_w = LCD_WIDTH * scale
        lcd_h = LCD_HEIGHT * scale
        self.case = pygame.Rect(MARGIN, MARGIN + STRAP_HEIGHT, PCB_WIDTH + lcd_w + 44, lcd_h + 44)
        self.lcd_rect = pygame.Rect(self.case.right - lcd_w - 22, self.case.top + 22, lcd_w, lcd_h)
        self.pcb = pygame.Rect(self.case.left + 12, self.case.top + 12, PCB_WIDTH, self.case.height - 24)

        height = max(self.case.bottom + STRAP_HEIGHT + MARGIN, 620)
        width = self.case.right + MARGIN + PANEL_WIDTH + MARGIN
        self.screen = pygame.display.set_mode((width, height))
        pygame.display.set_caption(title)
        self.panel = pygame.Rect(self.case.right + MARGIN, MARGIN, PANEL_WIDTH, height - 2 * MARGIN)

        self.font = pygame.font.Font(None, 22)
        self.small = pygame.font.Font(None, 18)
        self.heading = pygame.font.Font(None, 28)

        self.lcd = pygame.Surface((LCD_WIDTH, LCD_HEIGHT), depth=16, masks=(0xF800, 0x07E0, 0x001F, 0))
        self.seen_seq = -1
        self.buzzer = Buzzer(sound)
        self.clock = pygame.time.Clock()

        self.pitch = 0.0
        self.roll = 0.0
        self.tilt_dragging = False
        self.battery_dragging = False
        self.last_motion_time = time.monotonic()
        self.status = ""
        self.status_until = 0.0
        self.pressed: set[str] = set()

        # Hit zones on the illustration.
        cx = self.pcb.left + 62
        cy = self.pcb.bottom - 62
        self.lever_center = (cx, cy)
        self.lever_left = pygame.Rect(cx - 52, cy - 34, 34, 68)
        self.lever_push = pygame.Rect(cx - 18, cy - 34, 36, 68)
        self.lever_right = pygame.Rect(cx + 18, cy - 34, 34, 68)
        self.side_button = pygame.Rect(self.case.left - 6, self.case.top + 54, 12, 36)
        self.usb_port = pygame.Rect(self.case.left - 6, self.case.top + 136, 46, 54)
        self.mouse_button: Optional[str] = None

        self._layout_panel()
        self.watch.bridge.set_imu((0.0, 0.0, 1000.0))

    # ------------------------------------------------------------------

    def _layout_panel(self) -> None:
        p = self.panel
        x = p.left + 14
        w = p.width - 28
        y = p.top + 112

        self.tilt_pad = pygame.Rect(x + (w - 150) // 2, y + 22, 150, 150)
        y = self.tilt_pad.bottom + 12

        third = (w - 12) // 3
        half = (w - 6) // 2
        self.buttons = [
            _Button("Tap", pygame.Rect(x, y, third, 28), lambda: self.watch.tap()),
            _Button("Double tap", pygame.Rect(x + third + 6, y, third, 28),
                    lambda: self.watch.tap(double=True)),
            _Button("+10 steps", pygame.Rect(x + 2 * (third + 6), y, third, 28),
                    lambda: self.watch.steps(10)),
        ]
        y += 62
        self.battery_slider = pygame.Rect(x, y, w, 10)
        y += 22
        self.buttons += [
            _Button("USB cable", pygame.Rect(x, y, half, 28), self._toggle_usb,
                    lambda: self.watch.usb),
            _Button("Charging", pygame.Rect(x + half + 6, y, half, 28), self._toggle_charging,
                    lambda: self.watch.charging),
        ]
        y += 40
        self.buttons += [
            _Button("Reset", pygame.Rect(x, y, third, 28), self.watch.reset),
            _Button("Screenshot", pygame.Rect(x + third + 6, y, third, 28), self.save_screenshot),
            _Button("Mute", pygame.Rect(x + 2 * (third + 6), y, third, 28), self._toggle_mute,
                    lambda: self.buzzer.muted),
        ]
        self.help_top = y + 40

    def _toggle_usb(self) -> None:
        usb = not self.watch.usb
        self.watch.set_battery(usb=usb, charging=usb and self.watch.charging)

    def _toggle_charging(self) -> None:
        charging = not self.watch.charging
        self.watch.set_battery(usb=self.watch.usb or charging, charging=charging)

    def _toggle_mute(self) -> None:
        self.buzzer.muted = not self.buzzer.muted

    def save_screenshot(self) -> None:
        name = time.strftime("artemis-%Y%m%d-%H%M%S.png")
        path = os.path.join(self.screenshot_dir, name)
        state = self.watch.state
        if state.frame:
            save_png(path, state.frame, LCD_WIDTH, LCD_HEIGHT, 3)
            self._flash_status(f"Saved {name}")

    def _flash_status(self, text: str) -> None:
        self.status = text
        self.status_until = time.monotonic() + 3

    # ------------------------------------------------------------------
    # Input

    KEYS = {
        pygame.K_LEFT: "down",
        pygame.K_RIGHT: "up",
        pygame.K_RETURN: "select",
        pygame.K_SPACE: "select",
        pygame.K_ESCAPE: "back",
        pygame.K_BACKSPACE: "back",
    }

    def _set_tilt(self, pitch: float, roll: float) -> None:
        now = time.monotonic()
        dt = max(now - self.last_motion_time, 0.005)
        gyro = ((pitch - self.pitch) / dt, (roll - self.roll) / dt, 0.0)
        self.pitch = max(-90.0, min(90.0, pitch))
        self.roll = max(-90.0, min(90.0, roll))
        self.last_motion_time = now
        p, r = math.radians(self.pitch), math.radians(self.roll)
        accel = (math.sin(r) * math.cos(p), math.sin(p), math.cos(r) * math.cos(p))
        self.watch.set_motion(accel, gyro)

    def _tilt_from_mouse(self, pos: tuple[int, int]) -> None:
        pad = self.tilt_pad
        self._set_tilt(
            -((pos[1] - pad.centery) / (pad.height / 2)) * 90,
            ((pos[0] - pad.centerx) / (pad.width / 2)) * 90,
        )

    def _battery_from_mouse(self, x: int) -> None:
        s = self.battery_slider
        fraction = max(0.0, min(1.0, (x - s.left) / s.width))
        self.watch.set_battery(mv=int(3200 + fraction * 1000))

    def _lever_at(self, pos: tuple[int, int]) -> Optional[str]:
        if self.lever_left.collidepoint(pos):
            return "down"
        if self.lever_right.collidepoint(pos):
            return "up"
        if self.lever_push.collidepoint(pos):
            return "select"
        if self.side_button.collidepoint(pos):
            return "back"
        return None

    def _press(self, name: str) -> None:
        self.pressed.add(name)
        self.watch.set_button(name, True)

    def _release(self, name: str) -> None:
        self.pressed.discard(name)
        self.watch.set_button(name, False)

    def handle_event(self, event: pygame.event.Event) -> bool:
        if event.type == pygame.QUIT:
            return False

        if event.type == pygame.KEYDOWN:
            if event.key in self.KEYS:
                self._press(self.KEYS[event.key])
            elif event.key == pygame.K_F12:
                self.save_screenshot()
            elif event.key == pygame.K_m:
                self._toggle_mute()
            elif event.key == pygame.K_r:
                self.watch.reset()
            elif event.key == pygame.K_t:
                self.watch.tap()
            elif event.key == pygame.K_0:
                self._set_tilt(0, 0)
            elif event.key in (pygame.K_w, pygame.K_s, pygame.K_a, pygame.K_d):
                self._set_tilt(
                    self.pitch + {pygame.K_w: 10, pygame.K_s: -10}.get(event.key, 0),
                    self.roll + {pygame.K_a: -10, pygame.K_d: 10}.get(event.key, 0),
                )
        elif event.type == pygame.KEYUP and event.key in self.KEYS:
            self._release(self.KEYS[event.key])

        elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
            name = self._lever_at(event.pos)
            if name:
                self.mouse_button = name
                self._press(name)
            elif self.usb_port.collidepoint(event.pos):
                self._toggle_usb()
            elif self.tilt_pad.collidepoint(event.pos):
                self.tilt_dragging = True
                self._tilt_from_mouse(event.pos)
            elif self.battery_slider.inflate(0, 16).collidepoint(event.pos):
                self.battery_dragging = True
                self._battery_from_mouse(event.pos[0])
            else:
                for button in self.buttons:
                    if button.rect.collidepoint(event.pos):
                        button.action()
        elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 3:
            if self.tilt_pad.collidepoint(event.pos):
                self._set_tilt(0, 0)
        elif event.type == pygame.MOUSEBUTTONUP and event.button == 1:
            if self.mouse_button:
                self._release(self.mouse_button)
                self.mouse_button = None
            self.tilt_dragging = False
            self.battery_dragging = False
        elif event.type == pygame.MOUSEMOTION:
            if self.tilt_dragging:
                self._tilt_from_mouse(event.pos)
            elif self.battery_dragging:
                self._battery_from_mouse(event.pos[0])
        return True

    # ------------------------------------------------------------------
    # Drawing

    def _text(self, text: str, pos: tuple[int, int], font=None, color=TEXT, center=False) -> None:
        surface = (font or self.font).render(text, True, color)
        rect = surface.get_rect(center=pos) if center else surface.get_rect(topleft=pos)
        self.screen.blit(surface, rect)

    def _led(self, center: tuple[int, int], color: tuple[int, int, int], amount: float, r: int = 6) -> None:
        amount = max(0.0, min(1.0, amount))
        if amount > 0.02:
            glow = pygame.Surface((r * 8, r * 8), pygame.SRCALPHA)
            for i in range(4, 0, -1):
                pygame.draw.circle(glow, (*color, int(46 * amount * (5 - i) / 4)), (r * 4, r * 4), r + i * r // 2)
            self.screen.blit(glow, (center[0] - r * 4, center[1] - r * 4))
        pygame.draw.circle(self.screen, _lit(color, amount), center, r)
        pygame.draw.circle(self.screen, (20, 20, 22), center, r, 1)

    def _draw_strap(self) -> None:
        strap = pygame.Rect(self.lcd_rect.left + 40, 0, self.lcd_rect.width - 80, self.screen.get_height())
        pygame.draw.rect(self.screen, STRAP, strap)
        for i in range(3):
            x = strap.left + 28 + i * 34
            y = self.case.top - 40
            pygame.draw.polygon(
                self.screen, ACCENT,
                [(x, y + 20), (x + 14, y + 20), (x + 28, y), (x + 14, y)],
            )

    def _draw_pcb(self) -> None:
        screen = self.screen
        p = self.pcb
        pygame.draw.rect(screen, PCB, p, border_radius=6)

        # The ESP32-S3 module.
        module = pygame.Rect(p.left + 54, p.top + 16, 108, 96)
        pygame.draw.rect(screen, METAL, module, border_radius=3)
        self._text("ESP32-S3", (module.centerx, module.top + 30), self.small, (40, 40, 44), center=True)
        self._text("MINI-1 N4R2", (module.centerx, module.top + 50), self.small, (70, 70, 76), center=True)

        # USB-C: click to plug and unplug.
        usb = self.usb_port
        pygame.draw.rect(screen, METAL, usb, border_radius=8)
        pygame.draw.rect(screen, (40, 42, 46), usb.inflate(-14, -30), border_radius=4)
        if self.watch.usb:
            cable = pygame.Rect(usb.left - 6, usb.centery - 10, 26, 20)
            pygame.draw.rect(screen, ACCENT, cable, border_radius=3)
        self._text("USB", (usb.centerx + 4, usb.bottom + 12), self.small, TEXT_DIM, center=True)

        # Piezo buzzer.
        freq, duty = self.watch.buzzer
        center = (p.right - 46, p.top + 176)
        pygame.draw.circle(screen, (8, 8, 9), center, 30)
        pygame.draw.circle(screen, (60, 62, 66), center, 30, 2)
        pygame.draw.circle(screen, (30, 30, 32), center, 6)
        if freq:
            for r in (36, 42):
                pygame.draw.circle(screen, ACCENT, center, r, 1)
            self._text(f"{freq} Hz", (center[0], center[1] - 50), self.small, ACCENT, center=True)

        # Lever switch: left, right, and push down in the middle.
        cx, cy = self.lever_center
        offset = 0
        if "down" in self.pressed:
            offset = -14
        elif "up" in self.pressed:
            offset = 14
        pygame.draw.rect(screen, METAL, (cx - 34, cy - 18, 68, 36), border_radius=8)
        radius = 13 if "select" in self.pressed else 16
        knob = (cx + offset, cy)
        pygame.draw.circle(screen, (70, 72, 78), knob, radius + 2)
        pygame.draw.circle(screen, (118, 122, 130), knob, radius)
        self._text("<", (cx - 45, cy), self.font, TEXT_DIM, center=True)
        self._text(">", (cx + 45, cy), self.font, TEXT_DIM, center=True)
        self._text("lever", (cx, cy + 38), self.small, TEXT_DIM, center=True)

        # Side button.
        pressed = "back" in self.pressed
        button = self.side_button.move(4 if pressed else 0, 0)
        pygame.draw.rect(screen, (200, 40, 36), button, border_radius=3)
        self._text("button", (button.right + 26, button.centery), self.small, TEXT_DIM, center=True)

    def _draw_lcd(self) -> None:
        state = self.watch.state
        if state.frame and state.frame_seq != self.seen_seq:
            self.seen_seq = state.frame_seq
            self.lcd.get_buffer().write(state.frame, 0)

        r = self.lcd_rect
        pygame.draw.rect(self.screen, (8, 8, 8), r.inflate(14, 14), border_radius=4)
        if not state.powered:
            pygame.draw.rect(self.screen, (0, 0, 0), r)
            self._text("powered off", r.center, self.font, (70, 70, 74), center=True)
            self._text("press the button to turn on", (r.centerx, r.centery + 22), self.small,
                       (70, 70, 74), center=True)
            return

        scaled = pygame.transform.scale(self.lcd, r.size)
        self.screen.blit(scaled, r)
        brightness = self.watch.backlight
        if brightness < 0.995:
            shade = pygame.Surface(r.size)
            # The panel's image stays faintly visible with the backlight off.
            shade.set_alpha(int(255 * (1 - max(brightness, 0.02)) ** 1.5))
            self.screen.blit(shade, r)

    def _draw_leds(self) -> None:
        r = self.lcd_rect
        leds = self.watch.leds
        # Bottom edge LED1, LED2; right edge LED3, LED4; top edge LED5, LED6.
        self._led((r.left + r.width // 3, r.bottom + 12), LED_ICE_BLUE, leds[0])
        self._led((r.right - r.width // 3, r.bottom + 12), LED_ICE_BLUE, leds[1])
        self._led((r.right + 12, r.bottom - r.height // 3), LED_ORANGE, leds[2])
        self._led((r.right + 12, r.top + r.height // 3), LED_ORANGE, leds[3])
        self._led((r.right - r.width // 3, r.top - 12), LED_ICE_BLUE, leds[4])
        self._led((r.left + r.width // 3, r.top - 12), LED_ICE_BLUE, leds[5])

        # The RGB status LED sits to the left of the screen.
        red, green, blue = self.watch.rgb_led
        amount = max(red, green, blue)
        color = (int(red * 255), int(green * 255), int(blue * 255)) if amount else (255, 255, 255)
        peak = max(color) or 255
        color = tuple(c * 255 // peak for c in color)
        self._led((r.left - 12, r.centery), color, amount, 6)

    def _draw_panel(self) -> None:
        p = self.panel
        pygame.draw.rect(self.screen, PANEL, p, border_radius=8)
        pygame.draw.rect(self.screen, PANEL_EDGE, p, 1, border_radius=8)
        x = p.left + 14

        self._text("Artemis Watch 2.0", (x, p.top + 12), self.heading, ACCENT)
        state = self.watch.state
        self._text(f"hardware revision {state.revision}", (x, p.top + 38), self.small, TEXT_DIM)
        power = "on" if state.powered else "off"
        self._text(f"power {power}   backlight {self.watch.backlight * 100:.0f}%",
                   (x, p.top + 58), self.small, TEXT)
        self._text(f"{self.clock.get_fps():.0f} fps   display frame {state.frame_seq}",
                   (x, p.top + 76), self.small, TEXT_DIM)

        # Tilt pad.
        pad = self.tilt_pad
        self._text(f"tilt: pitch {self.pitch:+.0f}, roll {self.roll:+.0f}", (x, pad.top - 20), self.small, TEXT)
        pygame.draw.rect(self.screen, BACKGROUND, pad, border_radius=6)
        pygame.draw.rect(self.screen, PANEL_EDGE, pad, 1, border_radius=6)
        pygame.draw.line(self.screen, PANEL_EDGE, pad.midleft, pad.midright)
        pygame.draw.line(self.screen, PANEL_EDGE, pad.midtop, pad.midbottom)
        dot = (pad.centerx + int(self.roll / 90 * pad.width / 2),
               pad.centery - int(self.pitch / 90 * pad.height / 2))
        pygame.draw.circle(self.screen, ACCENT, dot, 7)

        # Battery.
        s = self.battery_slider
        self._text(f"battery: {self.watch.battery_mv / 1000:.2f} V", (x, s.top - 20), self.small, TEXT)
        pygame.draw.rect(self.screen, BACKGROUND, s, border_radius=5)
        fraction = (self.watch.battery_mv - 3200) / 1000
        pygame.draw.rect(self.screen, ACCENT,
                         (s.left, s.top, int(s.width * max(0.0, min(1.0, fraction))), s.height),
                         border_radius=5)
        knob_x = s.left + int(s.width * max(0.0, min(1.0, fraction)))
        pygame.draw.circle(self.screen, TEXT, (knob_x, s.centery), 8)

        mouse = pygame.mouse.get_pos()
        for button in self.buttons:
            active = button.active()
            fill = ACCENT if active else (56, 60, 68) if button.rect.collidepoint(mouse) else (46, 50, 57)
            pygame.draw.rect(self.screen, fill, button.rect, border_radius=5)
            self._text(button.label, button.rect.center, self.small,
                       (20, 20, 20) if active else TEXT, center=True)

        y = self.help_top
        for line in (
            "Left / Right   lever",
            "Enter or Space   push lever",
            "Esc   side button",
            "W A S D   tilt, 0   level",
            "T tap, R reset, M mute, F12 screenshot",
        ):
            self._text(line, (x, y), self.small, TEXT_DIM)
            y += 17

        if time.monotonic() < self.status_until:
            self._text(self.status, (x, p.bottom - 20), self.small, ACCENT)

    def render(self) -> None:
        self.screen.fill(BACKGROUND)
        self._draw_strap()
        pygame.draw.rect(self.screen, CASE, self.case, border_radius=18)
        pygame.draw.rect(self.screen, CASE_EDGE, self.case, 2, border_radius=18)
        self._draw_pcb()
        self._draw_lcd()
        self._draw_leds()
        self._draw_panel()

    def run(self, should_stop: Optional[Callable[[], bool]] = None) -> None:
        running = True
        while running and self.watch.running:
            for event in pygame.event.get():
                running = self.handle_event(event) and running
            if should_stop and should_stop():
                break
            freq, duty = self.watch.buzzer
            self.buzzer.set(freq, duty)
            self.render()
            pygame.display.flip()
            self.clock.tick(60)
        pygame.quit()

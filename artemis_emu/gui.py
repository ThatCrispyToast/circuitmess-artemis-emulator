"""The watch on your desktop: its display and LEDs, with ordinary controls for the rest."""

from __future__ import annotations

import array
import ctypes
import math
import os
import signal
import subprocess
import sys
import threading
import time
from typing import Callable, Optional

from PySide6.QtCore import QLibraryInfo, QRect, Qt, QTimer, qInstallMessageHandler
from PySide6.QtGui import QColor, QFontDatabase, QImage, QPainter, QPalette
from PySide6.QtWidgets import (
    QAbstractButton,
    QApplication,
    QCheckBox,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLayout,
    QMainWindow,
    QPushButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from .image import save_png
from .machine import LCD_HEIGHT, LCD_WIDTH, Watch

# The LEDs around the display: 1, 2, 5 and 6 are ice blue, 3 and 4 orange.
LED_COLORS = [(120, 225, 255), (120, 225, 255), (255, 150, 30), (255, 150, 30),
              (120, 225, 255), (120, 225, 255)]

BATTERY_MV = (3200, 4200)
TILT_STEP = 10


def _qt_message(kind, context, message: str) -> None:
    """Qt's messages as it would print them, less those about media back ends not used here."""
    category = context.category or "default"
    if category.startswith("qt.multimedia") or message.startswith("No QtMultimedia backends"):
        return
    print(message if category == "default" else f"{category}: {message}", file=sys.stderr)


def display_problem() -> Optional[str]:
    """
    Why a window can't be opened here, if it can't: Qt's own way of saying so is
    to abort the program.
    """
    if os.environ.get("QT_QPA_PLATFORM"):
        return None
    directory = os.path.join(QLibraryInfo.path(QLibraryInfo.LibraryPath.PluginsPath), "platforms")
    plugins = []
    if os.environ.get("WAYLAND_DISPLAY"):
        plugins += sorted(name for name in os.listdir(directory) if name.startswith("libqwayland"))
    if os.environ.get("DISPLAY"):
        plugins.append("libqxcb.so")
    if not plugins:
        return "there is no display to open it on"
    problem = None
    for name in plugins:
        try:
            ctypes.CDLL(os.path.join(directory, name))
            return None
        except OSError as error:
            problem = problem or missing_library(error)
    return problem


def missing_library(error: Exception) -> str:
    return (f"this system lacks a library Qt needs ({str(error).split(':')[0]}); "
            "docs/install.md lists the packages")


class Buzzer:
    """Plays what's on the piezo's pin as a square wave."""

    RATE = 44100
    # How much sound is kept queued: enough to bridge two refreshes of the window.
    AHEAD = RATE * 2 * 40 // 1000
    # Asks for the sound card, as opening it does. A sound server that is there and
    # not answering keeps Qt waiting half a minute, so another process waits instead.
    PROBE = ("import sys\n"
             "from PySide6.QtCore import QCoreApplication\n"
             "from PySide6.QtMultimedia import QMediaDevices\n"
             "app = QCoreApplication([])\n"
             "sys.exit(QMediaDevices.defaultAudioOutput().isNull())\n")
    PROBE_SECONDS = 5.0

    def __init__(self, enabled: bool = True):
        self.muted = not enabled
        self.available = False
        self._sink = None
        self._device = None
        self._probe: Optional[subprocess.Popen] = None
        self._probe_until = 0.0
        self._asked = False
        self._freq = 0
        self._phase = 0.0

    def _find_output(self) -> None:
        """Opens the sound card once the probe has found one, without ever waiting."""
        if not self._asked:
            self._asked = True
            self._probe_until = time.monotonic() + self.PROBE_SECONDS
            try:
                self._probe = subprocess.Popen(
                    [sys.executable, "-c", self.PROBE],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except OSError:
                pass
        if self._probe is None:
            return
        found = self._probe.poll()
        if found is None:
            if time.monotonic() < self._probe_until:
                return
            print("No sound: the sound server isn't answering.", file=sys.stderr, flush=True)
        self.close()
        if found == 0:
            self._open()

    def _open(self) -> None:
        try:
            from PySide6.QtMultimedia import QAudioFormat, QAudioSink, QMediaDevices

            output = QMediaDevices.defaultAudioOutput()
            if not output.isNull():
                sound = QAudioFormat()
                sound.setSampleRate(self.RATE)
                sound.setChannelCount(1)
                sound.setSampleFormat(QAudioFormat.SampleFormat.Int16)
                self._sink = QAudioSink(output, sound)
                self._sink.setBufferSize(4 * self.AHEAD)
                self._device = self._sink.start()
                self.available = self._device is not None
        except (ImportError, RuntimeError):
            pass

    def close(self) -> None:
        """Ends the probe, if it is still going."""
        if self._probe is not None:
            if self._probe.poll() is None:
                self._probe.kill()
            self._probe.wait()
            self._probe = None

    def set(self, freq: int, duty: float) -> None:
        audible = not self.muted and 20 <= freq <= 20000 and duty > 0
        self._freq = freq if audible else 0

    def pump(self) -> None:
        """Tops up what the sound card has queued. Called at every refresh."""
        if not self.available and not self.muted:
            self._find_output()
        if not self.available or not self._freq:
            return
        queued = self._sink.bufferSize() - self._sink.bytesFree()
        count = (self.AHEAD - queued) // 2
        if count <= 0:
            return
        step = self._freq / self.RATE
        phase = self._phase
        samples = array.array("h", (6000 if (phase + i * step) % 1 < 0.5 else -6000
                                    for i in range(count)))
        self._phase = (phase + count * step) % 1
        self._device.write(samples.tobytes())


class Display(QWidget):
    """The LCD, enlarged without smoothing and dimmed by its backlight."""

    BORDER = 6

    def __init__(self, watch: Watch, scale: int):
        super().__init__()
        self.watch = watch
        self.lcd = QRect(self.BORDER, self.BORDER, LCD_WIDTH * scale, LCD_HEIGHT * scale)
        self.setFixedSize(self.lcd.width() + 2 * self.BORDER, self.lcd.height() + 2 * self.BORDER)
        self._seen = -1
        self._frame = b""
        self._image: Optional[QImage] = None

    def refresh(self) -> None:
        state = self.watch.state
        if state.frame and state.frame_seq != self._seen:
            self._seen = state.frame_seq
            # QImage draws from this buffer, so it has to stay alive alongside it.
            self._frame = bytes(state.frame)
            self._image = QImage(self._frame, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH * 2,
                                 QImage.Format.Format_RGB16)
        self.update()

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), Qt.GlobalColor.black)
        if not self.watch.state.powered:
            painter.setPen(QColor(110, 110, 110))
            painter.drawText(self.lcd, Qt.AlignmentFlag.AlignCenter,
                             "Powered off\nPress the side button to turn it on")
            return
        if self._image is None:
            return
        painter.drawImage(self.lcd, self._image)
        brightness = self.watch.backlight
        if brightness < 0.995:
            # The panel's image stays faintly visible with the backlight off.
            shade = int(255 * (1 - max(brightness, 0.02)) ** 1.5)
            painter.fillRect(self.lcd, QColor(0, 0, 0, shade))


class Lamp(QWidget):
    """One LED: a dot that takes the LED's colour as it gets brighter."""

    SIZE = 14

    def __init__(self, color: tuple[int, int, int]):
        super().__init__()
        self.color = color
        self.amount = 0.0
        self.setFixedSize(self.SIZE, self.SIZE)

    def set(self, amount: float, color: Optional[tuple[int, int, int]] = None) -> None:
        amount = max(0.0, min(1.0, amount))
        color = color or self.color
        if (amount, color) != (self.amount, self.color):
            self.amount, self.color = amount, color
            self.update()

    def shown(self) -> QColor:
        off = self.palette().color(QPalette.ColorRole.Mid)
        mix = [round(o + (c - o) * self.amount)
               for o, c in zip((off.red(), off.green(), off.blue()), self.color)]
        return QColor(*mix)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(self.palette().color(QPalette.ColorRole.Dark))
        painter.setBrush(self.shown())
        painter.drawEllipse(1, 1, self.SIZE - 2, self.SIZE - 2)


class WatchWindow(QMainWindow):
    KEYS = {
        Qt.Key.Key_Left: "down",
        Qt.Key.Key_Right: "up",
        Qt.Key.Key_Return: "select",
        Qt.Key.Key_Enter: "select",
        Qt.Key.Key_Space: "select",
        Qt.Key.Key_Escape: "back",
        Qt.Key.Key_Backspace: "back",
    }
    HINT = "Arrows and Enter: lever   Esc: side button   W A S D: tilt"

    def __init__(
        self,
        watch: Watch,
        scale: int = 3,
        sound: bool = True,
        title: str = "Artemis Watch emulator",
        screenshot_dir: str = ".",
    ):
        qInstallMessageHandler(_qt_message)
        self._app = QApplication.instance() or QApplication(sys.argv[:1])
        if not QFontDatabase.families():
            print("No fonts are installed, so the window's labels will be blank: "
                  "install one, such as DejaVu Sans.", file=sys.stderr, flush=True)
        super().__init__()
        self.watch = watch
        self.screenshot_dir = screenshot_dir
        self.setWindowTitle(title)

        self.buzzer = Buzzer(sound)
        self.pitch = 0.0
        self.roll = 0.0
        self.last_motion_time = time.monotonic()
        self._should_stop: Optional[Callable[[], bool]] = None
        self._signal: Optional[int] = None

        self.display = Display(watch, scale)
        self.lamps = [Lamp(color) for color in LED_COLORS]
        self.rgb_lamp = Lamp((255, 255, 255))
        self.controls: dict[str, QAbstractButton] = {}

        left = QVBoxLayout()
        left.addWidget(self.display)
        left.addLayout(self._lamp_row())
        left.addStretch()

        right = QVBoxLayout()
        right.addWidget(self._watch_group())
        right.addWidget(self._buttons_group())
        right.addWidget(self._motion_group())
        right.addWidget(self._power_group())
        right.addLayout(self._bottom_row())
        right.addStretch()

        columns = QHBoxLayout()
        columns.addLayout(left)
        columns.addLayout(right)
        # In the layout, so that the window is as wide as any font makes it.
        self.hint = QLabel(self.HINT)
        layout = QVBoxLayout()
        layout.addLayout(columns)
        layout.addWidget(self.hint)
        layout.setSizeConstraint(QLayout.SizeConstraint.SetFixedSize)
        central = QWidget()
        central.setLayout(layout)
        self.setCentralWidget(central)

        # Keys go to the watch, not to whichever control was clicked last.
        for child in self.findChildren(QWidget):
            if isinstance(child, (QAbstractButton, QSlider)):
                child.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self.watch.bridge.set_imu((0.0, 0.0, 1000.0))
        self.refresh()

    # ------------------------------------------------------------------
    # Layout

    def _button(self, label: str, tip: str = "") -> QPushButton:
        button = QPushButton(label)
        if tip:
            button.setToolTip(tip)
        self.controls[label] = button
        return button

    def _checkbox(self, label: str, tip: str = "") -> QCheckBox:
        box = QCheckBox(label)
        if tip:
            box.setToolTip(tip)
        self.controls[label] = box
        return box

    def _lamp_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.addWidget(QLabel("LEDs"))
        for number, lamp in enumerate(self.lamps, 1):
            row.addSpacing(6)
            row.addWidget(lamp)
            row.addWidget(QLabel(str(number)))
        row.addStretch()
        row.addWidget(self.rgb_lamp)
        row.addWidget(QLabel("RGB"))
        return row

    def _watch_group(self) -> QGroupBox:
        self.power_label = QLabel()
        self.backlight_label = QLabel()
        self.buzzer_label = QLabel()
        form = QFormLayout()
        form.addRow("Power", self.power_label)
        form.addRow("Backlight", self.backlight_label)
        form.addRow("Buzzer", self.buzzer_label)
        group = QGroupBox(f"Watch, hardware revision {self.watch.state.revision}")
        group.setLayout(form)
        return group

    def _buttons_group(self) -> QGroupBox:
        grid = QGridLayout()
        held = (("Left", "down", "Left arrow", 0, 0), ("Push", "select", "Enter or Space", 0, 1),
                ("Right", "up", "Right arrow", 0, 2))
        for label, name, tip, row, column in held:
            button = self._button(label, tip)
            button.pressed.connect(lambda name=name: self.watch.set_button(name, True))
            button.released.connect(lambda name=name: self.watch.set_button(name, False))
            grid.addWidget(button, row, column)
        side = self._button("Side button", "Esc or Backspace")
        side.pressed.connect(lambda: self.watch.set_button("back", True))
        side.released.connect(lambda: self.watch.set_button("back", False))
        grid.addWidget(side, 1, 0, 1, 3)
        group = QGroupBox("Lever and button")
        group.setLayout(grid)
        return group

    def _tilt_slider(self) -> QSlider:
        slider = QSlider(Qt.Orientation.Horizontal)
        slider.setRange(-90, 90)
        slider.setPageStep(TILT_STEP)
        slider.valueChanged.connect(self._tilt_from_sliders)
        return slider

    def _motion_group(self) -> QGroupBox:
        self.pitch_slider = self._tilt_slider()
        self.roll_slider = self._tilt_slider()
        self.pitch_label = QLabel()
        self.roll_label = QLabel()
        for label in (self.pitch_label, self.roll_label):
            label.setMinimumWidth(label.fontMetrics().horizontalAdvance("-90°"))
            label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        grid = QGridLayout()
        grid.addWidget(QLabel("Pitch"), 0, 0)
        grid.addWidget(self.pitch_slider, 0, 1)
        grid.addWidget(self.pitch_label, 0, 2)
        grid.addWidget(QLabel("Roll"), 1, 0)
        grid.addWidget(self.roll_slider, 1, 1)
        grid.addWidget(self.roll_label, 1, 2)
        grid.setColumnStretch(1, 1)

        level = self._button("Level", "0")
        level.clicked.connect(lambda: self._set_tilt(0, 0))
        tap = self._button("Tap", "T")
        tap.clicked.connect(lambda: self.watch.tap())
        double = self._button("Double tap")
        double.clicked.connect(lambda: self.watch.tap(double=True))
        steps = self._button("+10 steps")
        steps.clicked.connect(lambda: self.watch.steps(10))
        row = QHBoxLayout()
        for button in (level, tap, double, steps):
            row.addWidget(button)

        box = QVBoxLayout()
        box.addLayout(grid)
        box.addLayout(row)
        group = QGroupBox("Motion")
        group.setLayout(box)
        return group

    def _power_group(self) -> QGroupBox:
        self.battery_slider = QSlider(Qt.Orientation.Horizontal)
        self.battery_slider.setRange(*BATTERY_MV)
        self.battery_slider.setPageStep(100)
        self.battery_slider.setValue(self.watch.battery_mv)
        self.battery_slider.valueChanged.connect(lambda mv: self.watch.set_battery(mv=mv))
        self.battery_label = QLabel()

        usb = self._checkbox("USB cable")
        usb.clicked.connect(self._set_usb)
        charging = self._checkbox("Charging")
        charging.clicked.connect(self._set_charging)

        grid = QGridLayout()
        grid.addWidget(QLabel("Battery"), 0, 0)
        grid.addWidget(self.battery_slider, 0, 1)
        grid.addWidget(self.battery_label, 0, 2)
        grid.setColumnStretch(1, 1)
        boxes = QHBoxLayout()
        boxes.addWidget(usb)
        boxes.addWidget(charging)
        boxes.addStretch()
        grid.addLayout(boxes, 1, 0, 1, 3)
        group = QGroupBox("Power")
        group.setLayout(grid)
        return group

    def _bottom_row(self) -> QHBoxLayout:
        reset = self._button("Reset", "R. Cuts the power and restores it.")
        reset.clicked.connect(self.watch.reset)
        shot = self._button("Screenshot", "F12")
        shot.clicked.connect(self.save_screenshot)
        mute = self._checkbox("Mute", "M")
        mute.clicked.connect(self._set_mute)
        row = QHBoxLayout()
        row.addWidget(reset)
        row.addWidget(shot)
        row.addStretch()
        row.addWidget(mute)
        return row

    # ------------------------------------------------------------------
    # What the controls do

    def _set_usb(self, plugged: bool) -> None:
        self.watch.set_battery(usb=plugged, charging=plugged and self.watch.charging)
        self._show_switches()

    def _set_charging(self, charging: bool) -> None:
        self.watch.set_battery(usb=self.watch.usb or charging, charging=charging)
        self._show_switches()

    def _set_mute(self, muted: bool) -> None:
        self.buzzer.muted = muted
        self._show_switches()

    def _show_switches(self) -> None:
        self.controls["USB cable"].setChecked(self.watch.usb)
        self.controls["Charging"].setChecked(self.watch.charging)
        self.controls["Mute"].setChecked(self.buzzer.muted)

    def save_screenshot(self) -> None:
        name = time.strftime("artemis-%Y%m%d-%H%M%S.png")
        state = self.watch.state
        if state.frame:
            save_png(os.path.join(self.screenshot_dir, name), state.frame, LCD_WIDTH, LCD_HEIGHT, 3)
            self.hint.setText(f"Saved {name}")
            QTimer.singleShot(3000, lambda: self.hint.setText(self.HINT))

    def _set_tilt(self, pitch: float, roll: float) -> None:
        now = time.monotonic()
        dt = max(now - self.last_motion_time, 0.005)
        pitch = max(-90.0, min(90.0, pitch))
        roll = max(-90.0, min(90.0, roll))
        gyro = ((pitch - self.pitch) / dt, (roll - self.roll) / dt, 0.0)
        self.pitch, self.roll = pitch, roll
        self.last_motion_time = now
        p, r = math.radians(pitch), math.radians(roll)
        accel = (math.sin(r) * math.cos(p), math.sin(p), math.cos(r) * math.cos(p))
        self.watch.set_motion(accel, gyro)

        for slider, value in ((self.pitch_slider, pitch), (self.roll_slider, roll)):
            slider.blockSignals(True)
            slider.setValue(round(value))
            slider.blockSignals(False)
        self.pitch_label.setText(f"{pitch:+.0f}°")
        self.roll_label.setText(f"{roll:+.0f}°")

    def _tilt_from_sliders(self) -> None:
        self._set_tilt(self.pitch_slider.value(), self.roll_slider.value())

    def keyPressEvent(self, event) -> None:
        key = event.key()
        if key in self.KEYS:
            if not event.isAutoRepeat():
                self.watch.set_button(self.KEYS[key], True)
        elif key == Qt.Key.Key_F12:
            self.save_screenshot()
        elif key == Qt.Key.Key_M:
            self._set_mute(not self.buzzer.muted)
        elif key == Qt.Key.Key_R:
            self.watch.reset()
        elif key == Qt.Key.Key_T:
            self.watch.tap()
        elif key == Qt.Key.Key_0:
            self._set_tilt(0, 0)
        elif key in (Qt.Key.Key_W, Qt.Key.Key_S):
            self._set_tilt(self.pitch + (TILT_STEP if key == Qt.Key.Key_W else -TILT_STEP), self.roll)
        elif key in (Qt.Key.Key_A, Qt.Key.Key_D):
            self._set_tilt(self.pitch, self.roll + (TILT_STEP if key == Qt.Key.Key_D else -TILT_STEP))
        else:
            super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:
        if event.key() in self.KEYS:
            if not event.isAutoRepeat():
                self.watch.set_button(self.KEYS[event.key()], False)
        else:
            super().keyReleaseEvent(event)

    # ------------------------------------------------------------------
    # Showing the watch

    def refresh(self) -> None:
        watch = self.watch
        self.display.refresh()
        for lamp, amount in zip(self.lamps, watch.leds):
            lamp.set(amount)
        red, green, blue = watch.rgb_led
        peak = max(red, green, blue)
        if peak > 0:
            self.rgb_lamp.set(peak, tuple(round(c / peak * 255) for c in (red, green, blue)))
        else:
            self.rgb_lamp.set(0)

        self.power_label.setText("on" if watch.state.powered else "off")
        self.backlight_label.setText(f"{watch.backlight * 100:.0f}%")
        freq, duty = watch.buzzer
        self.buzzer_label.setText(f"{freq} Hz" if freq and duty > 0 else "quiet")
        self.buzzer.set(freq, duty)
        self.buzzer.pump()

        self.battery_label.setText(f"{watch.battery_mv / 1000:.2f} V")
        if not self.battery_slider.isSliderDown():
            self.battery_slider.blockSignals(True)
            self.battery_slider.setValue(watch.battery_mv)
            self.battery_slider.blockSignals(False)
        self._show_switches()
        self.pitch_label.setText(f"{self.pitch:+.0f}°")
        self.roll_label.setText(f"{self.roll:+.0f}°")

    def _tick(self) -> None:
        stop = self._should_stop
        if self._signal is not None or not self.watch.running or (stop and stop()):
            self.close()
            return
        self.refresh()

    def closeEvent(self, event) -> None:
        self._timer.stop()
        self.buzzer.close()
        super().closeEvent(event)
        self._app.quit()

    def run(self, should_stop: Optional[Callable[[], bool]] = None) -> None:
        """Shows the window until it is closed, the watch goes away, or `should_stop` says so."""
        self._should_stop = should_stop

        # A signal's handler can't unwind Qt's event loop, so it only leaves a note,
        # and the signal is delivered again once the loop has ended.
        caught = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
        saved = {}
        if threading.current_thread() is threading.main_thread():
            for number in caught:
                saved[number] = signal.signal(number, lambda n, frame: setattr(self, "_signal", n))
        try:
            self.show()
            self._timer.start(16)
            self._app.exec()
        finally:
            self._timer.stop()
            for number, handler in saved.items():
                signal.signal(number, handler)
        if self._signal is not None:
            number, self._signal = self._signal, None
            signal.raise_signal(number)

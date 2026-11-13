"""Command line for the Artemis Watch emulator."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import select
import signal
import sys
import termios
import threading
import time
import tty
from pathlib import Path
from typing import Optional

from .device import Device, DeviceError
from .firmware import (STOCK_VERSIONS, FirmwareNotFound, describe, micropython_firmware,
                       stock_firmware)
from .flash import FLASH_SIZE, FirmwareError, flash_plan
from .machine import EmulatorError, Watch, write_to_stdout
from .micropython import PythonError, PythonSession, WatchRestarted

_NO_MICROPYTHON = (
    "MicroPython didn't answer on the watch. `artemis-emu device flash micropython` "
    "puts it there."
)


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--headless", action="store_true", help="run without a window")
    parser.add_argument("--timeout", type=float, metavar="SECONDS",
                        help="stop after this long (headless runs go on until Ctrl-C otherwise)")
    parser.add_argument("--screenshot", metavar="PNG", help="save the display to a file on exit")
    parser.add_argument("--scale", type=int, default=3, help="display zoom in the window (default 3)")
    parser.add_argument("--mute", action="store_true", help="don't play the buzzer")
    parser.add_argument("--revision", type=int, choices=(2, 3), default=3,
                        help="hardware revision (default 3, the current one)")
    parser.add_argument("--battery", type=int, default=3900, metavar="MV",
                        help="battery voltage in millivolts (default 3900)")
    parser.add_argument("--usb", action="store_true", help="start with USB plugged in")
    parser.add_argument("--charging", action="store_true", help="start charging")
    parser.add_argument("--time", metavar="ISO8601",
                        help="set the RTC, such as 2026-07-20T20:17:00 (default: local time), "
                             "or `unset` for a clock that was never set")
    parser.add_argument("--flash", metavar="FILE",
                        help="keep flash contents in this file between runs: settings, files, "
                             "NVS. It's created from the firmware on first use")
    parser.add_argument("--turbo", action="store_true",
                        help="run code as fast as this computer can, not at the watch's speed")
    parser.add_argument("--sensor-noise", action="store_true",
                        help="give motion sensor readings the noise a real one has")
    parser.add_argument("--no-watchdogs", action="store_true",
                        help="never let a timer group watchdog fire, as when debugging a hang")
    parser.add_argument("--gdb", type=int, metavar="PORT", help="open a GDB stub on this port")
    parser.add_argument("--wait-gdb", action="store_true", help="don't start until GDB continues")


def _add_device(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device", nargs="?", const="", metavar="PORT",
                        help="use the watch plugged into this computer instead of the emulator; "
                             "PORT is only needed when there are several")


def _start_time(text: Optional[str]):
    if not text:
        return None
    if text == "unset":
        return text
    try:
        return dt.datetime.fromisoformat(text)
    except ValueError:
        raise SystemExit(f"--time: {text!r} isn't a date and time like 2026-07-20T20:17:00, or `unset`")


def _make_watch(args: argparse.Namespace, firmware) -> Watch:
    if args.wait_gdb and not args.gdb:
        raise SystemExit("--wait-gdb needs --gdb PORT")
    return Watch(
        firmware,
        revision=args.revision,
        battery_mv=args.battery,
        usb=args.usb or args.charging,
        charging=args.charging,
        time=_start_time(args.time),
        flash_file=args.flash,
        console=write_to_stdout,
        gdb_port=args.gdb,
        wait_for_gdb=args.wait_gdb,
        watchdogs=not args.no_watchdogs,
        turbo=args.turbo,
        sensor_noise=args.sensor_noise,
    )


def _forward_stdin(watch: Watch) -> None:
    """Sends typed lines to the firmware's serial console."""
    if not sys.stdin.isatty():
        return

    def pump() -> None:
        for line in sys.stdin.buffer:
            if not watch.running:
                return
            watch.write_serial(line.replace(b"\n", b"\r\n"))

    threading.Thread(target=pump, daemon=True).start()


def _run_window(watch: Watch, args: argparse.Namespace, title: str, should_stop=None) -> None:
    # Imported late so that headless use needs no graphics stack.
    from .gui import WatchWindow

    window = WatchWindow(watch, scale=args.scale, sound=not args.mute, title=title)
    deadline = time.monotonic() + args.timeout if args.timeout else None

    def stop() -> bool:
        if deadline and time.monotonic() > deadline:
            return True
        return bool(should_stop and should_stop())

    window.run(stop)


def _wait_headless(watch, timeout: Optional[float], should_stop=None) -> None:
    deadline = time.monotonic() + timeout if timeout else None
    while watch.running:
        if deadline and time.monotonic() > deadline:
            return
        if should_stop and should_stop():
            return
        time.sleep(0.05)


def _finish(watch: Watch, args: argparse.Namespace) -> None:
    if args.screenshot and watch.running:
        watch.screenshot(args.screenshot)


def _terminal_to(port, running, stop: threading.Event) -> None:
    """Raw terminal, so Ctrl-C and friends reach the watch. Ctrl-] leaves."""
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    tty.setraw(fd)
    try:
        while running():
            if select.select([fd], [], [], 0.1)[0]:
                data = os.read(fd, 256)
                if b"\x1d" in data:
                    break
                port.write(data)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        stop.set()


def _device_session(device: Device) -> PythonSession:
    """Gets MicroPython's attention on a real watch, unsticking its input if need be."""
    try:
        return PythonSession(device, boot_timeout=3)
    except (TimeoutError, EmulatorError):
        device.recover()
        return PythonSession(device, boot_timeout=10)


def cmd_run(args: argparse.Namespace, firmware, title: str) -> int:
    with _make_watch(args, firmware) as watch:
        _forward_stdin(watch)
        try:
            if args.headless:
                _wait_headless(watch, args.timeout)
            else:
                _run_window(watch, args, title)
        except KeyboardInterrupt:
            pass
        _finish(watch, args)
    return 0


def _run_script(watch, args: argparse.Namespace, code: str, result: dict, stopping: threading.Event,
                show_console) -> None:
    """Runs or installs the script; the same on an emulated watch and a real one."""
    try:
        if isinstance(watch, Device):
            session = _device_session(watch)
        else:
            session = PythonSession(watch)
        if args.install:
            session.write_file("main.py", code.encode())
            print(f"Installed {args.script} as main.py; restarting MicroPython.", flush=True)
            show_console()
            session.soft_reset()
        else:
            session.run(code, on_output=write_to_stdout)
    except PythonError as e:
        print(e.traceback, file=sys.stderr, flush=True)
        result["failed"] = True
    except WatchRestarted as e:
        print(f"error: {e} before the script finished. Scripts only outlive a "
              "restart with --install.", file=sys.stderr, flush=True)
        result["failed"] = True
    except (EmulatorError, DeviceError, TimeoutError) as e:
        # Stopping the watch ourselves ends the session too.
        if not stopping.is_set():
            message = _NO_MICROPYTHON if isinstance(watch, Device) and "never answered" in str(e) else e
            print(f"error: {message}", file=sys.stderr, flush=True)
            result["failed"] = True


def cmd_python(args: argparse.Namespace) -> int:
    code = Path(args.script).read_text()
    result: dict = {}
    done = threading.Event()
    stopping = threading.Event()

    if args.device is not None:
        watch_context = Device(args.device or None)
    else:
        watch_context = _make_watch(args, args.firmware or micropython_firmware())

    with watch_context as watch:
        # The script's own output is forwarded by the session, not the boot chatter.
        watch._console = None

        def show_console() -> None:
            watch._console = write_to_stdout

        def worker() -> None:
            _run_script(watch, args, code, result, stopping, show_console)
            done.set()

        threading.Thread(target=worker, daemon=True).start()

        try:
            if args.device is not None:
                # The port closes and reopens around a recovery, so go by the script alone.
                finished = done.wait if not args.install else threading.Event().wait
                finished(args.timeout)
            elif args.headless:
                # Without a window, a finished script means we're done.
                stop = (lambda: False) if args.install else done.is_set
                _wait_headless(watch, args.timeout, stop)
            else:
                _run_window(watch, args, f"Artemis Watch 2.0 - {args.script}")
        except KeyboardInterrupt:
            pass
        still_running = not done.is_set()
        stopping.set()
        if args.device is None:
            _finish(watch, args)
        elif still_running and not args.install and watch.running:
            watch.write_serial(b"\x03\x03")  # don't leave the script running on the watch

    if still_running and args.timeout and not args.install:
        print(f"error: script still running after {args.timeout} s", file=sys.stderr)
        return 1
    return 1 if result.get("failed") else 0


def cmd_repl(args: argparse.Namespace) -> int:
    stop = threading.Event()

    if args.device is not None:
        with Device(args.device or None, console=write_to_stdout) as device:
            if sys.stdin.isatty():
                print(f"The watch on {device.port}. Ctrl-] quits.\r", flush=True)
                threading.Thread(target=_terminal_to, daemon=True,
                                 args=(device.usb_port, lambda: device.running, stop)).start()
                device.write_serial(b"\r")
            try:
                _wait_headless(device, args.timeout, stop.is_set)
            except KeyboardInterrupt:
                pass
        return 0

    with _make_watch(args, args.firmware or micropython_firmware()) as watch:
        if sys.stdin.isatty():
            print("MicroPython REPL on the emulated watch. Ctrl-] quits.\r", flush=True)
            threading.Thread(target=_terminal_to, daemon=True,
                             args=(watch.usb_port, lambda: watch.running, stop)).start()

        try:
            if args.headless:
                _wait_headless(watch, args.timeout, stop.is_set)
            else:
                _run_window(watch, args, "Artemis Watch 2.0 - REPL", stop.is_set)
        except KeyboardInterrupt:
            pass
        _finish(watch, args)
    return 0


# ----------------------------------------------------------------------
# A real watch


def cmd_device(args: argparse.Namespace) -> int:
    device = Device(args.port)

    if args.action == "info":
        info = device.info()
        print(f"Port:      {info['port']}")
        print(f"Chip:      {info['chip']}")
        print(f"           {info['features']}")
        print(f"MAC:       {info['mac']}")
        print(f"Flash:     {info['flash']}")
        if info["product"] == 3:
            print(f"Hardware:  Artemis Watch 2.0, revision {info['revision']}")
        else:
            print(f"Hardware:  not an Artemis Watch 2.0 (product {info['product']}, "
                  f"revision {info['revision']})")
        print(f"Firmware:  {info['firmware']}")
        for label, kind, subtype, offset, size in info["partitions"]:
            print(f"           {label:<10} at {offset:#08x}, {size // 1024} KB")
        return 0

    if args.action == "backup":
        target = Path(args.file)
        if target.exists():
            raise SystemExit(f"{target} already exists")
        print("Reading the watch's flash twice, about a minute...", flush=True)
        firmware = device.backup(target)
        print(f"Saved {target}: {firmware}.")
        return 0

    if args.action == "restore":
        image = Path(args.file).read_bytes()
        if len(image) != FLASH_SIZE:
            raise SystemExit(f"{args.file} isn't a whole-flash backup: it should be {FLASH_SIZE} bytes")
        print(f"Writing {describe(image)} back to the watch...", flush=True)
        device.write_flash(args.file, quiet=False)
        return 0

    if args.action == "flash":
        if args.what == "micropython":
            erased = device.install_micropython(micropython_firmware(), keep_files=not args.erase_files,
                                                quiet=False)
            print("MicroPython is on the watch" +
                  (", with an empty filesystem." if erased else "; its files were kept."))
        elif args.what == "stock":
            device.write_flash(stock_firmware(args.version), quiet=False)
            print("The stock firmware is on the watch, with its settings as new.")
        else:
            plan = flash_plan(args.what)
            arguments = [part for offset, path in plan for part in (hex(offset), str(path))]
            device.esptool("write-flash", *arguments, quiet=False)
        return 0

    # monitor
    stop = threading.Event()
    with Device(args.port, console=write_to_stdout) as watch:
        if args.reset:
            watch.reset()
        if sys.stdin.isatty():
            print(f"The watch on {watch.port}. Ctrl-] quits.\r", flush=True)
            threading.Thread(target=_terminal_to, daemon=True,
                             args=(watch.usb_port, lambda: watch.running, stop)).start()
        try:
            _wait_headless(watch, args.timeout, stop.is_set)
        except KeyboardInterrupt:
            pass
    return 0


def _exit_on_signal(signum: int, frame) -> None:
    # Unwinds through `with Watch`, so that the emulator's files get cleaned up.
    raise SystemExit(128 + signum)


def main(argv: Optional[list[str]] = None) -> int:
    signal.signal(signal.SIGTERM, _exit_on_signal)
    signal.signal(signal.SIGHUP, _exit_on_signal)

    parser = argparse.ArgumentParser(
        prog="artemis-emu",
        description="Run code on an emulated CircuitMess NASA Artemis Watch 2.0, or a real one.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    stock = commands.add_parser("stock", help="run CircuitMess's factory firmware")
    stock.add_argument("--version", choices=STOCK_VERSIONS, metavar="VERSION",
                       help=f"which release: {', '.join(STOCK_VERSIONS)} (default: the latest)")
    _add_common(stock)

    run = commands.add_parser("run", help="run a firmware you built")
    run.add_argument("firmware", help="merged flash image, app .bin, or ESP-IDF build directory")
    _add_common(run)

    python = commands.add_parser("python", help="run a MicroPython script on the watch")
    python.add_argument("script", help="the Python file to run")
    python.add_argument("--install", action="store_true",
                        help="store it as main.py and restart, as if it had been flashed")
    python.add_argument("--firmware", help="MicroPython image to use instead of the built-in one")
    _add_device(python)
    _add_common(python)

    repl = commands.add_parser("repl", help="open a MicroPython prompt on the watch")
    repl.add_argument("--firmware", help="MicroPython image to use instead of the built-in one")
    _add_device(repl)
    _add_common(repl)

    device = commands.add_parser("device", help="look after a real watch plugged into this computer")
    actions = device.add_subparsers(dest="action", required=True)

    def action(name: str, help: str) -> argparse.ArgumentParser:
        sub = actions.add_parser(name, help=help)
        sub.add_argument("--port", help="its serial port, when several are plugged in")
        return sub

    action("info", "say what the watch is and what's on it")
    backup = action("backup", "save everything in its flash to a file")
    backup.add_argument("file")
    restore = action("restore", "write a backup back")
    restore.add_argument("file")
    flash = action("flash", "put a firmware on it")
    flash.add_argument("what", metavar="micropython|stock|FIRMWARE",
                       help="CircuitMess's MicroPython, their factory firmware, or a build of yours")
    flash.add_argument("--version", choices=STOCK_VERSIONS, metavar="VERSION",
                       help="which stock release (default: the latest)")
    flash.add_argument("--erase-files", action="store_true",
                       help="with micropython: start with an empty filesystem")
    monitor = action("monitor", "show its serial console")
    monitor.add_argument("--reset", action="store_true", help="restart the watch first")
    monitor.add_argument("--timeout", type=float, metavar="SECONDS", help="stop after this long")

    args = parser.parse_args(argv)

    try:
        if args.command == "stock":
            name = f"stock firmware {args.version}" if args.version else "stock firmware"
            return cmd_run(args, stock_firmware(args.version), f"Artemis Watch 2.0 - {name}")
        if args.command == "run":
            return cmd_run(args, args.firmware, f"Artemis Watch 2.0 - {Path(args.firmware).name}")
        if args.command == "python":
            return cmd_python(args)
        if args.command == "repl":
            return cmd_repl(args)
        return cmd_device(args)
    except (EmulatorError, FirmwareError, FirmwareNotFound, DeviceError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

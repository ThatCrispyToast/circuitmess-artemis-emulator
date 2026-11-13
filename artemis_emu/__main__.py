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

from .flash import FirmwareError
from .machine import EmulatorError, Watch, write_to_stdout
from .micropython import PythonError, PythonSession, WatchRestarted


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--headless", action="store_true", help="run without a window")
    parser.add_argument("--timeout", type=float, metavar="SECONDS",
                        help="stop after this long (headless runs go on until Ctrl-C otherwise)")
    parser.add_argument("--screenshot", metavar="PNG", help="save the display to a file on exit")
    parser.add_argument("--scale", type=int, default=3, help="display zoom in the window (default 3)")
    parser.add_argument("--mute", action="store_true", help="don't play the buzzer")
    parser.add_argument("--revision", type=int, choices=(2, 3), default=3,
                        help="hardware revision (default 3, what's shipping)")
    parser.add_argument("--battery", type=int, default=3900, metavar="MV",
                        help="battery voltage in millivolts (default 3900)")
    parser.add_argument("--usb", action="store_true", help="start with USB plugged in")
    parser.add_argument("--charging", action="store_true", help="start charging")
    parser.add_argument("--time", metavar="ISO8601",
                        help="set the RTC, such as 2026-07-20T20:17:00 (default: local time)")
    parser.add_argument("--flash", metavar="FILE",
                        help="keep flash contents in this file between runs: settings, files, "
                             "NVS. It's created from the firmware on first use")
    parser.add_argument("--no-watchdogs", action="store_true",
                        help="never let a timer group watchdog fire, as when debugging a hang")
    parser.add_argument("--gdb", type=int, metavar="PORT", help="open a GDB stub on this port")
    parser.add_argument("--wait-gdb", action="store_true", help="don't start until GDB continues")


def _make_watch(args: argparse.Namespace, firmware: str) -> Watch:
    if args.wait_gdb and not args.gdb:
        raise SystemExit("--wait-gdb needs --gdb PORT")
    return Watch(
        firmware,
        revision=args.revision,
        battery_mv=args.battery,
        usb=args.usb or args.charging,
        charging=args.charging,
        time=dt.datetime.fromisoformat(args.time) if args.time else None,
        flash_file=args.flash,
        console=write_to_stdout,
        gdb_port=args.gdb,
        wait_for_gdb=args.wait_gdb,
        watchdogs=not args.no_watchdogs,
    )


def _firmware_from_env(variable: str, what: str) -> str:
    path = os.environ.get(variable)
    if not path:
        raise SystemExit(f"{what} isn't available: run through `nix run`, or set {variable}")
    return path


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


def _wait_headless(watch: Watch, args: argparse.Namespace, should_stop=None) -> None:
    deadline = time.monotonic() + args.timeout if args.timeout else None
    while watch.running:
        if deadline and time.monotonic() > deadline:
            return
        if should_stop and should_stop():
            return
        time.sleep(0.05)


def _finish(watch: Watch, args: argparse.Namespace) -> None:
    if args.screenshot and watch.running:
        watch.screenshot(args.screenshot)


def cmd_run(args: argparse.Namespace, firmware: str, title: str) -> int:
    with _make_watch(args, firmware) as watch:
        _forward_stdin(watch)
        try:
            if args.headless:
                _wait_headless(watch, args)
            else:
                _run_window(watch, args, title)
        except KeyboardInterrupt:
            pass
        _finish(watch, args)
    return 0


def cmd_python(args: argparse.Namespace) -> int:
    firmware = args.firmware or _firmware_from_env(
        "ARTEMIS_MICROPYTHON_FIRMWARE", "The MicroPython firmware"
    )
    code = Path(args.script).read_text()
    result: dict = {}
    done = threading.Event()
    stopping = threading.Event()

    with _make_watch(args, firmware) as watch:
        # The script's own output is forwarded by the session, not the boot chatter.
        watch._console = None

        def worker() -> None:
            try:
                session = PythonSession(watch)
                if args.install:
                    session.write_file("main.py", code.encode())
                    print(f"Installed {args.script} as main.py; rebooting.", flush=True)
                    watch._console = write_to_stdout
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
            except (EmulatorError, TimeoutError) as e:
                # Stopping the emulator ourselves ends the session too.
                if not stopping.is_set():
                    print(f"error: {e}", file=sys.stderr, flush=True)
                    result["failed"] = True
            done.set()

        threading.Thread(target=worker, daemon=True).start()

        try:
            if args.headless:
                # Without a window, a finished script means we're done.
                stop = (lambda: False) if args.install else done.is_set
                _wait_headless(watch, args, stop)
            else:
                _run_window(watch, args, f"Artemis Watch 2.0 - {args.script}")
        except KeyboardInterrupt:
            pass
        still_running = not done.is_set()
        stopping.set()
        _finish(watch, args)

    if still_running and args.timeout and not args.install:
        print(f"error: script still running after {args.timeout} s", file=sys.stderr)
        return 1
    return 1 if result.get("failed") else 0


def cmd_repl(args: argparse.Namespace) -> int:
    firmware = args.firmware or _firmware_from_env(
        "ARTEMIS_MICROPYTHON_FIRMWARE", "The MicroPython firmware"
    )
    with _make_watch(args, firmware) as watch:
        stop = threading.Event()

        def pump() -> None:
            # Raw terminal, so Ctrl-C and friends reach the REPL. Ctrl-] leaves.
            fd = sys.stdin.fileno()
            saved = termios.tcgetattr(fd)
            tty.setraw(fd)
            try:
                while watch.running:
                    if select.select([fd], [], [], 0.1)[0]:
                        data = os.read(fd, 256)
                        if b"\x1d" in data:
                            break
                        watch.usb_port.write(data)
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, saved)
                stop.set()

        if sys.stdin.isatty():
            print("MicroPython REPL on the emulated watch. Ctrl-] quits.\r", flush=True)
            threading.Thread(target=pump, daemon=True).start()

        try:
            if args.headless:
                _wait_headless(watch, args, stop.is_set)
            else:
                _run_window(watch, args, "Artemis Watch 2.0 - REPL", stop.is_set)
        except KeyboardInterrupt:
            pass
        _finish(watch, args)
    return 0


def _exit_on_signal(signum: int, frame) -> None:
    # Unwinds through `with Watch`, so that the emulator's files get cleaned up.
    raise SystemExit(128 + signum)


def main(argv: Optional[list[str]] = None) -> int:
    signal.signal(signal.SIGTERM, _exit_on_signal)
    signal.signal(signal.SIGHUP, _exit_on_signal)

    parser = argparse.ArgumentParser(
        prog="artemis-emu",
        description="Run code on an emulated CircuitMess NASA Artemis Watch 2.0.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    stock = commands.add_parser("stock", help="run CircuitMess's factory firmware")
    _add_common(stock)

    run = commands.add_parser("run", help="run a firmware you built")
    run.add_argument("firmware", help="merged flash image, app .bin, or ESP-IDF build directory")
    _add_common(run)

    python = commands.add_parser("python", help="run a MicroPython script on the watch")
    python.add_argument("script", help="the Python file to run")
    python.add_argument("--install", action="store_true",
                        help="store it as main.py and reboot, as if it had been flashed")
    python.add_argument("--firmware", help="MicroPython image to use instead of the built-in one")
    _add_common(python)

    repl = commands.add_parser("repl", help="open a MicroPython prompt on the watch")
    repl.add_argument("--firmware", help="MicroPython image to use instead of the built-in one")
    _add_common(repl)

    args = parser.parse_args(argv)

    try:
        if args.command == "stock":
            return cmd_run(
                args,
                _firmware_from_env("ARTEMIS_STOCK_FIRMWARE", "The stock firmware"),
                "Artemis Watch 2.0 - stock firmware",
            )
        if args.command == "run":
            return cmd_run(args, args.firmware, f"Artemis Watch 2.0 - {Path(args.firmware).name}")
        if args.command == "python":
            return cmd_python(args)
        return cmd_repl(args)
    except (EmulatorError, FirmwareError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

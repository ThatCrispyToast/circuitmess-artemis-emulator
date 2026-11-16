"""
The emulator's files that aren't Python: its QEMU, and the firmware it runs.

Each comes from where an environment variable says, which is how the Nix package
supplies them, or from this user's data directory, where it is put the first time
it is wanted: downloaded, or for QEMU built by qemu/tools/build.sh.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

# What is downloaded, from where, and the SHA-256 each file must have.
PINS = json.loads((Path(__file__).parent / "assets.json").read_text())


class AssetError(Exception):
    pass


def home() -> Path:
    """Where built and downloaded files are kept: ARTEMIS_HOME, or the user's data directory."""
    named = os.environ.get("ARTEMIS_HOME")
    if named:
        return Path(named)
    data = os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share"
    return Path(data) / "artemis-emu"


def _download(url: str, sha256: str, target: Path, what: str) -> Path:
    """Fetches a file unless it's already there, and refuses one that isn't the one pinned."""
    if target.exists():
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {what} from {url}", file=sys.stderr, flush=True)
    digest = hashlib.sha256()
    partial = tempfile.NamedTemporaryFile(dir=target.parent, prefix=".partial-", delete=False)
    try:
        with partial, urllib.request.urlopen(url, timeout=60) as reply:
            while chunk := reply.read(1 << 16):
                digest.update(chunk)
                partial.write(chunk)
        if digest.hexdigest() != sha256:
            raise AssetError(
                f"{url} isn't the file expected: its SHA-256 is {digest.hexdigest()}, not {sha256}"
            )
        os.replace(partial.name, target)
    except (urllib.error.URLError, OSError) as error:
        raise AssetError(f"couldn't download {what} from {url}: {error}") from None
    finally:
        if os.path.exists(partial.name):
            os.unlink(partial.name)
    return target


def _published(entry: dict, target: Path, what: str) -> Path:
    """A file from this project's release; ARTEMIS_ASSETS_URL names a mirror of it."""
    base = (os.environ.get("ARTEMIS_ASSETS_URL") or PINS["release"]).rstrip("/")
    return _download(f"{base}/{entry['file']}", entry["sha256"], target, what)


def system() -> str:
    """The kind of computer this is, as published builds are named."""
    return f"{sys.platform}-{platform.machine()}"


def qemu() -> Path:
    """The emulator's QEMU: the one ARTEMIS_QEMU names, one built here, or the published build."""
    named = os.environ.get("ARTEMIS_QEMU")
    if named:
        return Path(named)
    built = home() / "qemu" / "bin" / "qemu-system-xtensa"
    if built.exists():
        return built

    entry = PINS["qemu"].get(system())
    if entry is None:
        raise AssetError(
            f"there's no published build of the emulator's QEMU for {system()}: "
            "build it with qemu/tools/build.sh"
        )
    unpacked = home() / f"qemu-{entry['sha256'][:12]}"
    binary = unpacked / "bin" / "qemu-system-xtensa"
    if not binary.exists():
        try:
            archive = _published(entry, home() / entry["file"], "the emulator's QEMU")
        except AssetError as error:
            raise AssetError(f"{error}. qemu/tools/build.sh builds it instead") from None
        staging = Path(tempfile.mkdtemp(dir=home(), prefix=".unpack-"))
        try:
            with tarfile.open(archive) as tar:
                try:
                    tar.extractall(staging, filter="data")
                except TypeError:  # a Python from before the filters
                    tar.extractall(staging)
            shutil.rmtree(unpacked, ignore_errors=True)
            os.replace(staging, unpacked)
        finally:
            shutil.rmtree(staging, ignore_errors=True)
        archive.unlink()
    return binary


def micropython_firmware() -> Path:
    """This project's build of CircuitMess's MicroPython."""
    entry = PINS["micropython"]
    if "notices" in entry:
        notices = entry["notices"]
        _published(notices, home() / "firmware" / notices["file"], "MicroPython license notices")
    return _published(entry, home() / "firmware" / entry["file"], "MicroPython for the watch")


def stock_firmware(version: str) -> Path:
    """A release of CircuitMess's firmware, from where they publish it."""
    stock = PINS["stock"]
    return _download(
        stock["url"].format(version=version),
        stock["releases"][version]["sha256"],
        home() / "firmware" / f"Artemis-v{version}.bin",
        f"CircuitMess's firmware {version}",
    )

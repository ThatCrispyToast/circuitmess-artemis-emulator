"""Finding and fetching the emulator's QEMU and firmware, against a server of our own."""

import hashlib
import http.server
import io
import os
import tarfile
import threading

import pytest

from artemis_emu import assets, micropython_firmware, stock_firmware
from artemis_emu.firmware import FirmwareNotFound, describe, stock_release

NAMED = ("ARTEMIS_QEMU", "ARTEMIS_STOCK_FIRMWARE", "ARTEMIS_STOCK_FIRMWARE_DIR",
         "ARTEMIS_MICROPYTHON_FIRMWARE")


@pytest.fixture
def served(tmp_path, monkeypatch):
    """A directory served over HTTP as this project's release, and an empty home."""
    files = tmp_path / "served"
    files.mkdir()
    handler = lambda *args: http.server.SimpleHTTPRequestHandler(*args, directory=str(files))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    server.RequestHandlerClass.log_message = lambda *args: None
    threading.Thread(target=server.serve_forever, daemon=True).start()
    for variable in NAMED:
        monkeypatch.delenv(variable, raising=False)
    monkeypatch.setenv("ARTEMIS_HOME", str(tmp_path / "home"))
    monkeypatch.setenv("ARTEMIS_ASSETS_URL", f"http://127.0.0.1:{server.server_address[1]}")
    yield files
    server.shutdown()


def pin(monkeypatch, kind, data):
    """Makes `data` the file pinned for the QEMU bundle or the MicroPython image."""
    entry = {"file": f"{kind}.bin", "sha256": hashlib.sha256(data).hexdigest()}
    pins = dict(assets.PINS)
    pins[kind] = {assets.system(): entry} if kind == "qemu" else entry
    monkeypatch.setattr(assets, "PINS", pins)
    return entry


def bundle(*names):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:xz") as tar:
        for name in names:
            member = tarfile.TarInfo(name)
            member.size, member.mode = 4, 0o755
            tar.addfile(member, io.BytesIO(b"qemu"))
    return data.getvalue()


def test_firmware_is_fetched_once_and_kept(served, monkeypatch):
    entry = pin(monkeypatch, "micropython", b"an image")
    (served / entry["file"]).write_bytes(b"an image")
    path = micropython_firmware()
    assert path.read_bytes() == b"an image"
    assert path.parent == assets.home() / "firmware"

    (served / entry["file"]).unlink()
    assert micropython_firmware() == path


def test_file_that_is_not_the_one_pinned_is_refused(served, monkeypatch):
    entry = pin(monkeypatch, "micropython", b"an image")
    (served / entry["file"]).write_bytes(b"something else")
    with pytest.raises(FirmwareNotFound, match="isn't the file expected"):
        micropython_firmware()
    assert not list((assets.home() / "firmware").iterdir())


def test_missing_file_says_where_it_looked(served, monkeypatch):
    pin(monkeypatch, "micropython", b"an image")
    with pytest.raises(FirmwareNotFound, match="couldn't download .*127.0.0.1.*404"):
        micropython_firmware()


def test_named_files_are_used_as_they_are(served, monkeypatch, tmp_path):
    monkeypatch.setenv("ARTEMIS_MICROPYTHON_FIRMWARE", str(tmp_path / "mine.bin"))
    monkeypatch.setenv("ARTEMIS_STOCK_FIRMWARE", str(tmp_path / "theirs.bin"))
    monkeypatch.setenv("ARTEMIS_QEMU", str(tmp_path / "qemu"))
    assert micropython_firmware() == tmp_path / "mine.bin"
    assert stock_firmware() == tmp_path / "theirs.bin"
    assert assets.qemu() == tmp_path / "qemu"


def test_published_qemu_is_unpacked_where_it_runs_from(served, monkeypatch):
    data = bundle("bin/qemu-system-xtensa", "share/qemu/esp32s3_rev0_rom.bin")
    entry = pin(monkeypatch, "qemu", data)
    (served / entry["file"]).write_bytes(data)
    binary = assets.qemu()
    assert binary.read_bytes() == b"qemu" and os.access(binary, os.X_OK)
    assert (binary.parent.parent / "share" / "qemu" / "esp32s3_rev0_rom.bin").exists()
    assert not (assets.home() / entry["file"]).exists()

    (served / entry["file"]).unlink()
    assert assets.qemu() == binary


def test_qemu_built_here_comes_before_the_published_one(served, monkeypatch):
    pin(monkeypatch, "qemu", b"never fetched")
    built = assets.home() / "qemu" / "bin" / "qemu-system-xtensa"
    built.parent.mkdir(parents=True)
    built.write_bytes(b"built")
    assert assets.qemu() == built


def test_unpublished_qemu_points_at_the_build_script(served, monkeypatch):
    pin(monkeypatch, "qemu", b"not there")
    with pytest.raises(assets.AssetError, match="qemu/tools/build.sh"):
        assets.qemu()
    monkeypatch.setattr(assets, "PINS", dict(assets.PINS, qemu={}))
    with pytest.raises(assets.AssetError, match="no published build.*qemu/tools/build.sh"):
        assets.qemu()


def test_unknown_stock_version_is_named_with_the_ones_there_are(served):
    with pytest.raises(FirmwareNotFound, match="2.0, 2.1, 2.1.1, 2.2"):
        stock_firmware("9.9")


def test_stock_releases_are_recognised_without_their_files(served):
    stock = os.environ.get("ARTEMIS_TEST_STOCK")
    if not stock:
        pytest.skip("no stock firmware at hand")
    image = open(stock, "rb").read()
    assert stock_release(image) == "2.2"
    assert describe(image) == "CircuitMess stock firmware 2.2"

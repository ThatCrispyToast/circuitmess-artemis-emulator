"""Check first-use downloads and the installed command without source-tree imports."""

from pathlib import Path
import os
import subprocess
import tempfile


with tempfile.TemporaryDirectory(prefix="artemis-smoke-") as directory:
    root = Path(directory)
    env = dict(os.environ, ARTEMIS_HOME=str(root / "assets"))
    for name in ("ARTEMIS_QEMU", "ARTEMIS_MICROPYTHON_FIRMWARE", "ARTEMIS_STOCK_FIRMWARE", "ARTEMIS_STOCK_FIRMWARE_DIR", "PYTHONPATH"):
        env.pop(name, None)
    script = root / "hello.py"
    script.write_text("print('artemis smoke test')\n")
    result = subprocess.run(["artemis-emu", "python", str(script), "--headless", "--timeout", "30"], cwd=root, env=env, capture_output=True, text=True, timeout=90, check=True)
    assert result.stdout.strip() == "artemis smoke test", result.stdout
    assert "Downloading" in result.stderr, result.stderr
    screenshot = root / "stock.png"
    subprocess.run(["artemis-emu", "stock", "--headless", "--timeout", "15", "--screenshot", str(screenshot)], cwd=root, env=env, check=True, timeout=90)
    assert screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    gui = root / "gui.png"
    subprocess.run(["xvfb-run", "-a", "artemis-emu", "stock", "--mute", "--timeout", "5", "--screenshot", str(gui)], cwd=root, env=env, check=True, timeout=60)
    assert gui.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    print("Installed CLI, first-use downloads, MicroPython, stock firmware, and GUI screenshots passed.")

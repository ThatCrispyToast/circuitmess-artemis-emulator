#!/usr/bin/env python3
"""Archive the exact source inputs from the pinned MicroPython Nix derivation."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


root = Path(__file__).resolve().parent.parent
data = json.loads(subprocess.check_output(["nix", "derivation", "show", ".#micropython-firmware"], cwd=root))
derivations = data.get("derivations", data)
env = next(iter(derivations.values()))["env"]
trees = {"micropython": Path(env["src"])}
for source, target in re.findall(r"cp -r --no-preserve=mode (\S+)/\. (\S+)/", env["postPatch"]):
    trees[Path(target).name] = Path(source)
mdns = re.search(r"cp -r --no-preserve=mode (\S+)/components/mdns", env["postPatch"])
trees["esp-protocols"] = Path(mdns[1])
esptool = re.search(r"ln -s (\S+)/esptool", env["configurePhase"])
trees["esptool"] = Path(esptool[1])
toolchain = next(Path(p) for p in env["nativeBuildInputs"].split()
                 if (Path(p) / "share/licenses/gcc/COPYING.RUNTIME").is_file())
runtime_notices = {f"toolchain-notices/{name}": toolchain / "share/licenses" / name
                   for name in ("gcc", "newlib")}
release = root / "build/release"
release.mkdir(parents=True, exist_ok=True)
stem = "micropython-circuitmess-s3-02bf648"
notices = ["CircuitMess MicroPython firmware: upstream and third-party notices.\n",
           "Source trees and build definitions accompany this firmware in the release.\n"]
with tempfile.TemporaryDirectory(prefix="artemis-micropython-sources-") as directory:
    work = Path(directory)
    for name, source in sorted({**trees, **runtime_notices}.items()):
        shutil.copytree(source, work / name, symlinks=True)
        for path in sorted(source.rglob("*")):
            if path.is_file() and re.match(r"(?i)^(licen[cs]e|copying|notice|copyright|authors)([._-].*)?$", path.name):
                try:
                    text = path.read_text()
                except UnicodeDecodeError:
                    continue
                notices.append(f"\n{'=' * 72}\n{name}/{path.relative_to(source)}\n{'=' * 72}\n{text}\n")
    (work / "nix").mkdir()
    shutil.copyfile(root / "nix/micropython.nix", work / "nix/micropython.nix")
    shutil.copyfile(root / "flake.lock", work / "flake.lock")
    (work / "BUILDING.txt").write_text(
        "These source trees are the inputs to nix/micropython.nix for the released firmware.\n"
        "The ESP-IDF tree includes its pinned submodules. Upstream licenses are retained.\n"
        "The derivation records the compiler, source revisions, hashes, build commands,\n"
        "and compatibility patches. From the matching project checkout, build with:\n\n"
        "  nix build .#micropython-firmware\n\n"
        "To use these local sources, replace each matching fetchFromGitHub expression\n"
        "in nix/micropython.nix with the corresponding extracted directory.\n"
    )
    subprocess.run(["tar", "-C", str(work), "--sort=name", "--owner=0", "--group=0", "--numeric-owner", "--mtime=@0", "-cJf", str(release / f"{stem}-sources.tar.xz"), "."], check=True)
notice_file = release / f"{stem}-NOTICES.txt"
notice_file.write_text("".join(notices))
pins_file = root / "artemis_emu/assets.json"
pins = json.loads(pins_file.read_text())
pins["micropython"]["notices"] = {"file": notice_file.name, "sha256": hashlib.sha256(notice_file.read_bytes()).hexdigest()}
pins_file.write_text(json.dumps(pins, indent=2) + "\n")
print(f"Packaged {len(trees)} source trees and {len(notices) - 2} notice files.")

#!/usr/bin/env python3
"""Package Ubuntu/Debian library notices and corresponding QEMU sources."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def archive(source, destination):
    subprocess.run([
        "tar", "-C", str(source), "--sort=name", "--owner=0", "--group=0",
        "--numeric-owner", "--mtime=@0", "-cJf", str(destination), ".",
    ], check=True)


def main():
    root, qemu, stage, binary = map(lambda p: Path(p).resolve(), sys.argv[1:])
    notes = stage / "share/doc/qemu-artemis"
    records = []
    for library in sorted((stage / "lib").iterdir()):
        matches = output("dpkg-query", "-S", f"*/{library.name}").splitlines()
        owners = {line.rsplit(": ", 1)[0] for line in matches}
        if len(owners) != 1:
            raise RuntimeError(f"Ambiguous package owner for {library.name}: {owners}")
        package = owners.pop()
        fields = output("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}", package).split("\t")
        name, version, source, source_version = fields
        copyright_file = Path("/usr/share/doc") / name.split(":")[0] / "copyright"
        destination = notes / "libraries" / name.replace(":", "-")
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(copyright_file, destination / "copyright")
        records.append({
            "library": library.name, "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
            "package": name, "version": version, "source": source, "source_version": source_version,
        })
    shutil.copytree("/usr/share/common-licenses", notes / "common-licenses")
    (notes / "libraries.json").write_text(json.dumps(records, indent=2) + "\n")
    stem = binary.name.removesuffix(".tar.xz")
    with tempfile.TemporaryDirectory(prefix="artemis-sources-") as temporary:
        work = Path(temporary)
        libraries = work / "libraries"
        libraries.mkdir()
        shutil.copyfile(notes / "libraries.json", libraries / "libraries.json")
        packages = sorted({(r["source"], r["source_version"]) for r in records})
        for name, version in packages:
            subprocess.run(["apt-get", "source", "--download-only", "--only-source", f"{name}={version}"], cwd=libraries, check=True)
        (libraries / "BUILDING.txt").write_text(
            "These are the exact Ubuntu/Debian source packages for the bundled libraries.\n"
            "Extract each with dpkg-source -x PACKAGE.dsc, install its build dependencies\n"
            "with apt-get build-dep PACKAGE, and build with dpkg-buildpackage -b -uc -us.\n"
            "Package versions and binary hashes are recorded in libraries.json.\n"
        )
        archive(libraries, binary.with_name(stem + "-library-sources.tar.xz"))
        sources = work / "qemu-sources"
        sources.mkdir()
        shutil.copytree(qemu, sources / "qemu", ignore=lambda directory, names: {"build"} if Path(directory) == qemu else set())
        shutil.copytree(root / "qemu", sources / "artemis/qemu")
        shutil.copyfile(root / "LICENSE", sources / "artemis/LICENSE")
        (sources / "BUILDING.txt").write_text(
            "This is the patched QEMU source used for the accompanying binary.\n"
            "Build on Ubuntu 22.04 with the development packages listed in the project\n"
            "installation guide. From this directory, run in bash:\n\n"
            "  mapfile -t flags < artemis/qemu/configure-flags\n"
            "  cd qemu\n"
            "  ./configure \"${flags[@]}\"\n"
            "  ninja -C build\n\n"
            "The keycodemapdb dependency is included. Library sources are provided in\n"
            "the adjacent library-sources archive. Original licenses remain in the tree.\n"
        )
        archive(sources, binary.with_name(stem + "-sources.tar.xz"))


if __name__ == "__main__":
    main()

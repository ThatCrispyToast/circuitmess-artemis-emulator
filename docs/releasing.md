# Releases

Release assets include QEMU, its bundled libraries and their sources, and the MicroPython image with its sources and notices. Stock firmware stays on CircuitMess's release page.

## Build the assets

Commit the release changes before exporting the source tree. Build QEMU in the Ubuntu 22.04 container:

```sh
podman build -t artemis-release -f tools/ubuntu-release.Containerfile tools
release_source=$(mktemp -d)
git archive HEAD | tar -x -C "$release_source"
mkdir -p build/release
podman run --rm --network slirp4netns \
  -v "$release_source:/source:ro" \
  -v "$PWD/build/release:/output:rw" -w / artemis-release \
  bash -c 'mkdir -p /work/repo && cp -a /source/. /work/repo/ && cd /work/repo && qemu/tools/build.sh --bundle /output/qemu-artemis-9.2.2-20260417-linux-x86_64.tar.xz'
rm -rf "$release_source"
```

The bundle command also writes `-sources.tar.xz` and `-library-sources.tar.xz` archives. It fails if it cannot identify or fetch a bundled library's source package. Keep these archives with the binary; later Ubuntu package updates may remove old versions from the package index.

Build and package MicroPython from the same checkout:

```sh
nix build .#micropython-firmware --out-link build/micropython
cp build/micropython/firmware.bin build/release/micropython-circuitmess-s3-02bf648.bin
python3 tools/package-micropython-sources.py
```

The packaging script archives the source inputs already fetched by Nix and updates the notices hash in `artemis_emu/assets.json`. Update the QEMU and firmware hashes there if their binaries changed. Update the release URL and package version for a new release.

## Verify

Build the Python distribution and install its wheel in a fresh virtual environment:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install build
python -m build
pip install dist/*.whl pytest
pip check
python tools/smoke.py
python -m pytest -q tests
```

The smoke check needs the system libraries in the [installation guide](install.md), plus `xvfb` and `xauth`. It checks first-use downloads, MicroPython output, stock firmware, and a GUI screenshot from outside the checkout.

Before publishing, serve `build/release` with `python3 -m http.server --bind 127.0.0.1 --directory build/release 8000` in a separate terminal and set `ARTEMIS_ASSETS_URL=http://127.0.0.1:8000` for the checks. After publishing, repeat the smoke check without that variable to verify the public URLs.

Run the full suite on native Linux. GitHub Actions checks installation and functional tests on Ubuntu 22.04 / Python 3.10 and Ubuntu 24.04 / Python 3.12; its virtual machines are unsuitable for hardware timing comparisons. Containers used for testing need an init process (`podman run --init`) to reap orphaned processes during the parent-process cleanup tests.

## Publish

Generate checksums, commit the updated pins, and push:

```sh
(cd build/release && sha256sum *.tar.xz *.bin *-NOTICES.txt > SHA256SUMS)
```

Create a GitHub release at that commit and attach every file in `build/release`: the binary archives, firmware image, all corresponding source archives, notices, and `SHA256SUMS`. Confirm that each URL in `artemis_emu/assets.json` works without authentication. The repository and release must be public for first-use downloads to work.

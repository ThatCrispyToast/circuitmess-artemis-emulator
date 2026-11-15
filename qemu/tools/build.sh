#!/usr/bin/env bash
# Builds the emulator's QEMU with ordinary tools: Espressif's fork, at the release this
# project is written against, with the watch's devices and the patch from qemu/.
#
#   qemu/tools/build.sh                  build it and install it where the emulator looks
#   qemu/tools/build.sh --prefix DIR     install it in DIR instead
#   qemu/tools/build.sh --bundle FILE    pack it as FILE with the libraries it uses, to
#                                        run on other computers (needs patchelf too)
#
# Needs a C compiler, make, ninja, pkg-config, python3 with venv, curl and patch, and the
# development files of glib, libgcrypt, pixman, libslirp and zlib; docs/install.md has
# the packages by distribution. Sources and the build stay in build/ in this checkout.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)

version=9.2.2-20260417
source_sha256=113bbae7d27dd39682c424e06ae18b0ec023ba8e4ab6754647116336557132c9
keycodemapdb=f5772a62ec52591ff6870b7e8ef32482371f22c6
keycodemapdb_sha256=d014b53382dbb17b8196ad12f50de7f20d0ef1b9f7d54b0be51a6cbb14209195

prefix=${ARTEMIS_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/artemis-emu}/qemu
bundle=
while [ $# -gt 0 ]; do
    case $1 in
    --prefix) prefix=$(realpath -m "$2"); shift 2 ;;
    --bundle) bundle=$(realpath -m "$2"); shift 2 ;;
    *) sed -n '2,12s/^# \{0,1\}//p' "$0"; exit 1 ;;
    esac
done

fetch() {  # URL SHA256 FILE
    if [ ! -e "$3" ]; then
        echo "Downloading $1"
        curl -fL --progress-bar -o "$3.part" "$1"
        mv "$3.part" "$3"
    fi
    echo "$2  $3" | sha256sum --check --quiet -
}

work=$root/build
mkdir -p "$work"
cd "$work"
fetch "https://github.com/espressif/qemu/archive/refs/tags/esp-develop-$version.tar.gz" \
    $source_sha256 "qemu-$version.tar.gz"
fetch "https://gitlab.com/qemu-project/keycodemapdb/-/archive/$keycodemapdb/keycodemapdb-$keycodemapdb.tar.gz" \
    $keycodemapdb_sha256 "keycodemapdb-$keycodemapdb.tar.gz"

rm -rf qemu
mkdir -p qemu/subprojects/keycodemapdb
tar -xzf "qemu-$version.tar.gz" -C qemu --strip-components=1
tar -xzf "keycodemapdb-$keycodemapdb.tar.gz" -C qemu/subprojects/keycodemapdb --strip-components=1
cd qemu

# As nix/qemu.nix does: no softfloat tests, which want two more subprojects, and the
# system's libslirp as a shared library.
sed -i "/subdir('fp')/d" tests/meson.build
sed -i "/slirp_dep = dependency/,+2 s/static: true/static: false/" meson.build

cp -r "$root/qemu/overlay/." .
for change in "$root"/qemu/patches/*.patch; do
    patch -s -p1 --forward --no-backup-if-mismatch < "$change"
done

mapfile -t flags < "$root/qemu/configure-flags"
./configure "${flags[@]}"
ninja -C build

if [ -z "$bundle" ]; then
    rm -rf "$prefix"
    DESTDIR=$prefix ninja -C build install > /dev/null
    echo "Installed in $prefix"
    exit
fi

# A tree that runs from wherever it is unpacked: the program, the libraries it was built
# against other than the C library, and the one ROM the watch's chip needs.
stage=$work/bundle
rm -rf "$stage"
DESTDIR=$stage ninja -C build install > /dev/null
find "$stage" -mindepth 1 -maxdepth 1 ! -name bin ! -name share -exec rm -rf {} +
find "$stage/share" -mindepth 1 -maxdepth 1 ! -name qemu -exec rm -rf {} +
find "$stage/share/qemu" -mindepth 1 -maxdepth 1 ! -name esp32s3_rev0_rom.bin -exec rm -rf {} +
strip --strip-debug "$stage/bin/qemu-system-xtensa"

mkdir "$stage/lib"
ldd "$stage/bin/qemu-system-xtensa" | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' |
    grep -vE '/(ld-linux[^/]*|libc|libm|libdl|librt|libpthread|libresolv|libgcc_s)\.so' |
    xargs -r cp -L -t "$stage/lib"
# The old kind of search path, which a library's own dependencies are found by too.
patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$stage/bin/qemu-system-xtensa"

notes=$stage/share/doc/qemu-artemis
mkdir -p "$notes"
cp COPYING COPYING.LIB LICENSE "$notes"
cat > "$notes/README" <<NOTE
QEMU for the CircuitMess Artemis Watch emulator.

This is Espressif's QEMU fork, release esp-develop-$version
(https://github.com/espressif/qemu), with the devices and the patch in the qemu/
directory of https://github.com/ThatCrispyToast/circuitmess-artemis-emulator, built
by qemu/tools/build.sh there. QEMU is licensed under the GNU GPL, version 2; see
COPYING, COPYING.LIB and LICENSE here.

lib/ holds the libraries it was built against, unmodified, from the distribution it
was built on: GLib and its PCRE (LGPL), libgcrypt and libgpg-error (LGPL), pixman
(MIT), libslirp (BSD) and zlib. Their source is that distribution's.
NOTE

tar -C "$stage" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
    -cJf "$bundle" bin lib share
echo "Packed $bundle"
sha256sum "$bundle"

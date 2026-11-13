#!/usr/bin/env bash
# The QEMU patch is the difference between two trees: a/ holds the files as Espressif
# has them, b/ the same files as changed here. Edit b/, not the patch.
#
#   patch-trees.sh unpack DIR    make DIR/a and DIR/b from the pinned QEMU source and the patch
#   patch-trees.sh pack DIR      write the patch from them
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
patch=$root/qemu/patches/0001-artemis-machine.patch

case "${1:-}" in
unpack)
    dir=${2:?where to put the trees}
    source=$(nix build --no-link --print-out-paths "$root#qemu-artemis.src")
    mkdir -p "$dir/a" "$dir/b"
    grep '^--- a/' "$patch" | cut -c7- | while read -r file; do
        for tree in a b; do
            mkdir -p "$dir/$tree/$(dirname "$file")"
            if [ -e "$source/$file" ]; then
                cp --no-preserve=mode "$source/$file" "$dir/$tree/$file"
            fi
        done
    done
    patch -s -p1 -d "$dir/b" < "$patch"
    ;;
pack)
    dir=${2:?where the trees are}
    # diff exits with 1 when the trees differ, which they do.
    (cd "$dir" && diff -ruN a b || [ $? -eq 1 ]) |
        sed -E 's/^(---|\+\+\+) (\S+)\t.*/\1 \2/' > "$patch"
    ;;
*)
    sed -n '2,7s/^# \{0,1\}//p' "$0"
    exit 1
    ;;
esac

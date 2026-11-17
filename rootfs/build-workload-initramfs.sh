#!/bin/sh
# The guest init is static; no application shell or libraries are included.
set -eu
init=$1
output=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/dev" "$work/proc" "$work/sys" "$work/rootfs"
cp "$init" "$work/init"
chmod 755 "$work/init"
mkdir -p "$(dirname "$output")"
temporary=$output.tmp.$$
trap 'rm -rf "$work"; rm -f "$temporary"' EXIT HUP INT TERM
(cd "$work" && find . -print | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) |
    gzip -9 -n > "$temporary"
mv "$temporary" "$output"

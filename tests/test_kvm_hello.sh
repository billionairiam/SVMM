#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: Linux/KVM required'
    exit 0
fi

make all

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo 'SKIP: /dev/kvm is unavailable'
    exit 0
fi

output_file=$(mktemp)
trap 'rm -f "$output_file"' EXIT HUP INT TERM
timeout 5 ./build/stage-00-kvm-hello > "$output_file"
cmp tests/golden/stage-00.txt "$output_file"
echo 'PASS: Stage 00 KVM output'

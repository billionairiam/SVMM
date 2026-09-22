#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: Linux/KVM required'
    exit 0
fi

make all
make bin/test_serial
./bin/test_serial

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo 'SKIP: /dev/kvm is unavailable'
    exit 0
fi

output_file=$(mktemp)
trap 'rm -f "$output_file"' EXIT HUP INT TERM
./bin/serial_console > "$output_file"
if ! printf 'hello from guest\n' | cmp - "$output_file"; then
    echo 'unexpected serial output' >&2
    exit 1
fi
echo 'PASS: serial console'

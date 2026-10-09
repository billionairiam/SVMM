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
status=0
timeout 5 ./bin/serial_console > "$output_file" || status=$?
if [ "$status" -ne 0 ]; then
    if [ "$status" -eq 124 ]; then
        echo 'serial_console timed out' >&2
    else
        echo "serial_console exited with status $status" >&2
    fi
    exit 1
fi
if ! printf 'hello from guest\n' | cmp - "$output_file"; then
    echo 'unexpected serial output' >&2
    exit 1
fi
echo 'PASS: serial console'

#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
    echo 'SKIP: Linux/KVM required'
    exit 0
fi

make all bin/test_boot bin/test_uart bin/test_serial
./bin/test_boot
./bin/test_uart
./bin/test_serial

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    echo 'SKIP: /dev/kvm is unavailable'
    exit 0
fi

kernel=${BZIMAGE_PATH:-images/bzImage}
if [ ! -r "$kernel" ]; then
    echo 'SKIP: set BZIMAGE_PATH to a readable x86 bzImage for KVM test'
    exit 0
fi

output_file=$(mktemp)
error_file=$(mktemp)
trap 'rm -f "$output_file" "$error_file"' EXIT HUP INT TERM
timeout 30 ./bin/linux_boot "$kernel" > "$output_file" 2> "$error_file"
if ! grep -q 'Stage 02 completed' "$error_file"; then
    cat "$error_file" >&2
    echo 'missing Stage 02 completion marker' >&2
    exit 1
fi
if [ "${REQUIRE_KERNEL_LOG:-0}" = 1 ] &&
   ! grep -q 'Linux version' "$output_file"; then
    cat "$error_file" >&2
    echo 'kernel did not reach the serial console' >&2
    exit 1
fi
echo 'PASS: Stage 02 KVM run'

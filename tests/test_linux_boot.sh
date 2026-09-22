#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
    if [ -n "${BZIMAGE_PATH:-}" ] || [ "${REQUIRE_KERNEL_LOG:-0}" = 1 ]; then
        echo 'FAIL: Linux/KVM required' >&2
        exit 1
    fi
    echo 'SKIP: Linux/KVM required'
    exit 0
fi

make all bin/test_boot bin/test_uart bin/test_serial
./bin/test_boot
./bin/test_uart
./bin/test_serial

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    if [ -n "${BZIMAGE_PATH:-}" ] || [ "${REQUIRE_KERNEL_LOG:-0}" = 1 ]; then
        echo 'FAIL: /dev/kvm is unavailable' >&2
        exit 1
    fi
    echo 'SKIP: /dev/kvm is unavailable'
    exit 0
fi

kernel=${BZIMAGE_PATH:-images/bzImage}
if [ ! -r "$kernel" ]; then
    if [ -n "${BZIMAGE_PATH:-}" ] || [ "${REQUIRE_KERNEL_LOG:-0}" = 1 ]; then
        echo "FAIL: bzImage is unreadable: $kernel" >&2
        exit 1
    fi
    echo 'SKIP: set BZIMAGE_PATH to a readable x86 bzImage for KVM test'
    exit 0
fi

output_file=$(mktemp)
error_file=$(mktemp)
trap 'rm -f "$output_file" "$error_file"' EXIT HUP INT TERM
if timeout 15 ./bin/linux_boot "$kernel" > "$output_file" 2> "$error_file"; then
    result=0
else
    result=$?
fi
if [ "$result" -eq 0 ] && grep -q 'Stage 02 completed' "$error_file"; then
    :
elif [ "$result" -eq 124 ] &&
     grep -q 'Kernel panic - not syncing: VFS: Unable to mount root fs' "$output_file" &&
     grep -q 'Linux version' "$output_file"; then
    :
else
    cat "$error_file" >&2
    echo "unexpected KVM result: $result" >&2
    exit 1
fi
if [ "${REQUIRE_KERNEL_LOG:-0}" = 1 ] &&
   ! grep -q 'Linux version' "$output_file"; then
    cat "$error_file" >&2
    echo 'kernel did not reach the serial console' >&2
    exit 1
fi
echo 'PASS: Stage 02 KVM run'

#!/bin/sh
set -eu
kernel=${BZIMAGE_PATH:-/boot/vmlinuz-6.16.0}
[ -r "$kernel" ] || { echo "missing kernel: $kernel" >&2; exit 1; }
[ -r /dev/kvm ] && [ -w /dev/kvm ] || { echo '/dev/kvm unavailable' >&2; exit 1; }
make ablation

run_variant() {
    variant=$1
    out=$(mktemp)
    err=$(mktemp)
    if timeout 15 "bin/ablation/$variant/linux_boot" "$kernel" >"$out" 2>"$err"; then
        status=0
    else
        status=$?
    fi
    case "$variant" in
        baseline) grep -q 'Linux version' "$out" ;;
        no_uart) ! grep -q 'Linux version' "$out" ;;
        no_cpuid|no_protected_mode)
            [ "$status" -ne 0 ] || ! grep -q 'Linux version' "$out"
            ;;
    esac
    rm -f "$out" "$err"
}

run_variant baseline
run_variant no_cpuid
run_variant no_protected_mode
run_variant no_uart

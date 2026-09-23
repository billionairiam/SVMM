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
# 不加载 initramfs 时内核应停在 Stage 02 的边界：找不到根文件系统而 panic。
# 默认命令行带 panic=-1，内核随即复位，VMM 把复位当作客户机退出。
if INITRAMFS_PATH= timeout 30 ./bin/linux_boot "$kernel" \
       < /dev/null > "$output_file" 2> "$error_file"; then
    result=0
else
    result=$?
fi
if [ "$result" -ne 0 ] ||
   ! grep -q 'Stage 03 completed' "$error_file" ||
   ! grep -q 'stage=vcpu_exit reason=reset' "$error_file" ||
   ! grep -q 'Kernel panic - not syncing: VFS: Unable to mount root fs' "$output_file"; then
    cat "$error_file" >&2
    echo "unexpected KVM result without initramfs: $result" >&2
    exit 1
fi
if [ "${REQUIRE_KERNEL_LOG:-0}" = 1 ] &&
   ! grep -q 'Linux version' "$output_file"; then
    cat "$error_file" >&2
    echo 'kernel did not reach the serial console' >&2
    exit 1
fi
echo 'PASS: KVM run without initramfs reaches the root fs panic'

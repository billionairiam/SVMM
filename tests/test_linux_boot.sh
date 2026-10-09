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

make all bin/test_boot bin/test_uart bin/test_serial bin/test_rtc
./bin/test_boot
./bin/test_uart
./bin/test_serial
./bin/test_rtc

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
date_before=$(date -u +%Y-%m-%d)
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
date_after=$(date -u +%Y-%m-%d)

# 内核到达串口控制台时，检查它确实通过 CMOS RTC 读到了宿主机的 UTC 日期，
# 并且没有因为缺少 LAPIC 却仍宣称支持 APIC 而报错。
if grep -q 'Linux version' "$output_file"; then
    rtc_date=$(sed -n 's/.*PM: RTC time: [0-9:]*, date: \([0-9-]*\).*/\1/p' \
               "$output_file" | head -n 1)
    # 该行来自 CONFIG_PM_TRACE_RTC，裁剪过的内核可能没有，此时跳过日期比较。
    if [ -z "$rtc_date" ]; then
        echo 'NOTE: kernel did not log "PM: RTC time"; RTC date not checked'
    elif [ "$rtc_date" != "$date_before" ] && [ "$rtc_date" != "$date_after" ]; then
        grep 'RTC' "$output_file" >&2 || :
        echo "guest RTC date '$rtc_date' does not match host UTC date" >&2
        exit 1
    fi
    for message in 'Unable to read current time from RTC' 'Stale IRR' \
                   'APIC ID mismatch'; do
        if grep -q "$message" "$output_file"; then
            grep "$message" "$output_file" >&2
            echo "unexpected kernel message: $message" >&2
            exit 1
        fi
    done
fi
echo 'PASS: Stage 02 KVM run'

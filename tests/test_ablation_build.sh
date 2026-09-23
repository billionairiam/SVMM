#!/bin/sh
set -eu

variants='baseline no_cpuid fixed_32m no_boot_params no_e820 no_cmdline no_protected_mode no_uart'
make clean
make
test -x bin/linux_boot
test ! -e bin/ablation/baseline/linux_boot

make ablation
for variant in $variants; do
    test -x "bin/ablation/$variant/linux_boot"
    test -f "build/ablation/$variant/main.o"
    test -f "build/ablation/$variant/boot/linux.o"
done

tmp_object=$(mktemp)
trap 'rm -f "$tmp_object"' EXIT HUP INT TERM
if printf '#include "ablation.h"\n' |
   ${CC:-cc} -Isrc -DSVMM_ABLATE_CPUID -DSVMM_ABLATE_UART \
       -x c -c -o "$tmp_object" -; then
    echo 'multiple ablations were accepted' >&2
    exit 1
fi

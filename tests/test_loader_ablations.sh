#!/bin/sh
set -eu
mkdir -p bin/ablation-tests
for item in \
    'fixed_32m:SVMM_ABLATE_DYNAMIC_MEMORY' \
    'no_boot_params:SVMM_ABLATE_BOOT_PARAMS' \
    'no_e820:SVMM_ABLATE_E820' \
    'no_cmdline:SVMM_ABLATE_CMDLINE'
do
    name=${item%%:*}
    macro=${item#*:}
    ${CC:-cc} -Isrc -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE \
        -D"$macro" tests/test_boot.c src/boot/linux.c src/memory.c \
        -o "bin/ablation-tests/test_$name"
    "bin/ablation-tests/test_$name"
done

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE
CFLAGS += -pthread
CPPFLAGS += -Isrc
.DEFAULT_GOAL := all

SOURCES := src/main.c src/kvm.c src/memory.c src/vcpu.c src/serial.c src/console.c \
           src/metrics.c src/boot/linux.c src/boot/acpi.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)
DEPFILES := $(OBJECTS:.o=.d)

ABLATION_VARIANTS := baseline no_cpuid fixed_32m no_boot_params no_e820 \
                     no_cmdline no_protected_mode no_uart
ABLATION_CPPFLAGS_baseline :=
ABLATION_CPPFLAGS_no_cpuid := -DSVMM_ABLATE_CPUID
ABLATION_CPPFLAGS_fixed_32m := -DSVMM_ABLATE_DYNAMIC_MEMORY
ABLATION_CPPFLAGS_no_boot_params := -DSVMM_ABLATE_BOOT_PARAMS
ABLATION_CPPFLAGS_no_e820 := -DSVMM_ABLATE_E820
ABLATION_CPPFLAGS_no_cmdline := -DSVMM_ABLATE_CMDLINE
ABLATION_CPPFLAGS_no_protected_mode := -DSVMM_ABLATE_PROTECTED_MODE
ABLATION_CPPFLAGS_no_uart := -DSVMM_ABLATE_UART

ABLATION_DEPFILES :=

define ABLATION_template
ABLATION_OBJECTS_$(1) := $(SOURCES:src/%.c=build/ablation/$(1)/%.o)
ABLATION_DEPFILES += $$(ABLATION_OBJECTS_$(1):.o=.d)

bin/ablation/$(1)/linux_boot: $$(ABLATION_OBJECTS_$(1)) | bin/ablation/$(1)
	$$(CC) $$(CFLAGS) $$^ -o $$@

build/ablation/$(1)/%.o: src/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(ABLATION_CPPFLAGS_$(1)) \
		-DSVMM_VARIANT_NAME=\"$(1)\" -MMD -MP -c $$< -o $$@
endef

$(foreach variant,$(ABLATION_VARIANTS),$(eval $(call ABLATION_template,$(variant))))

-include $(DEPFILES) $(ABLATION_DEPFILES)

.PHONY: all ablation initramfs run test clean

all: bin/linux_boot

ablation: $(ABLATION_VARIANTS:%=bin/ablation/%/linux_boot)

initramfs: build/initramfs.cpio.gz

build/initramfs.cpio.gz: rootfs/build-rootfs.sh | build
	sh rootfs/build-rootfs.sh $@

bin/linux_boot: $(OBJECTS) | bin
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

bin/test_serial: tests/test_serial.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_serial.c src/serial.c -o $@

bin/test_uart: tests/test_uart.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_uart.c src/serial.c -o $@

bin/test_boot: tests/test_boot.c src/boot/linux.c src/boot/linux.h src/memory.c src/memory.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_boot.c src/boot/linux.c src/memory.c -o $@

bin/test_metrics: tests/test_metrics.c src/metrics.c src/metrics.h src/vcpu.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_metrics.c src/metrics.c -o $@

bin/test_vcpu_errors: tests/test_vcpu_errors.c src/vcpu.c src/vcpu.h src/boot/acpi.h \
                      src/serial.c src/serial.h src/metrics.c src/metrics.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_vcpu_errors.c src/vcpu.c \
		src/serial.c src/metrics.c -Wl,--wrap=ioctl -o $@

build/%.o: src/%.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build bin:
	mkdir -p $@

bin/ablation/%:
	mkdir -p $@

run: all initramfs
	@./bin/linux_boot

test: bin/test_metrics bin/test_vcpu_errors
	./bin/test_metrics
	./bin/test_vcpu_errors
	python3 -m unittest -v tests/test_ablation_collect.py
	sh tests/test_loader_ablations.sh
	sh tests/test_linux_boot.sh
	python3 -m unittest -v tests/test_initramfs_shell.py

clean:
	rm -rf build bin

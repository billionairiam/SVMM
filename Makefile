CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE
CPPFLAGS += -Isrc

SOURCES := src/main.c src/kvm.c src/memory.c src/vcpu.c src/serial.c src/boot/linux.c
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

.PHONY: all ablation run test clean

all: bin/linux_boot

ablation: $(ABLATION_VARIANTS:%=bin/ablation/%/linux_boot)

bin/linux_boot: $(OBJECTS) | bin
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

bin/test_serial: tests/test_serial.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_serial.c src/serial.c -o $@

bin/test_uart: tests/test_uart.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_uart.c src/serial.c -o $@

bin/test_boot: tests/test_boot.c src/boot/linux.c src/boot/linux.h src/memory.c src/memory.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_boot.c src/boot/linux.c src/memory.c -o $@

build/%.o: src/%.c | build
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build bin:
	mkdir -p $@

bin/ablation/%:
	mkdir -p $@

run: all
	@./bin/linux_boot

test:
	sh tests/test_linux_boot.sh

clean:
	rm -rf build bin

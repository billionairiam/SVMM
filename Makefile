CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE
CFLAGS += -pthread
CPPFLAGS += -Isrc
.DEFAULT_GOAL := all

SOURCES := src/main.c src/cli.c src/sandbox.c src/net-policy.c src/jail.c src/kvm.c src/memory.c \
           src/vcpu.c src/serial.c src/console.c src/metrics.c src/boot/linux.c src/boot/acpi.c \
           src/virtio/virtq.c src/virtio/mmio.c src/virtio/blk.c src/virtio/net.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)
# Stage 05 的 virtqueue 是纯用户态代码，不依赖 KVM，单独编进 virtq-demo。
# Stage 06 起 virtq.c 也编进 svmm，给 virtio-mmio 设备用。
VIRTIO_SOURCES := src/virtio/virtq.c src/virtio/virtio.c
VIRTIO_OBJECTS := $(VIRTIO_SOURCES:src/%.c=build/%.o)
BLK_SOURCES := src/virtio/virtq.c src/virtio/mmio.c src/virtio/blk.c
NET_SOURCES := src/virtio/virtq.c src/virtio/mmio.c src/virtio/net.c
DEPFILES := $(OBJECTS:.o=.d) $(VIRTIO_OBJECTS:.o=.d) build/virtio/demo.d

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

bin/ablation/$(1)/svmm: $$(ABLATION_OBJECTS_$(1)) | bin/ablation/$(1)
	$$(CC) $$(CFLAGS) $$^ -o $$@

build/ablation/$(1)/%.o: src/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CPPFLAGS) $$(CFLAGS) $$(ABLATION_CPPFLAGS_$(1)) \
		-DSVMM_VARIANT_NAME=\"$(1)\" -MMD -MP -c $$< -o $$@
endef

$(foreach variant,$(ABLATION_VARIANTS),$(eval $(call ABLATION_template,$(variant))))

-include $(DEPFILES) $(ABLATION_DEPFILES)

.PHONY: all ablation initramfs disk run run-disk run-net run-virtq test clean \
        netns netns-down run-netns jail-setup jail-clean run-jail help

# make run KERNEL=/boot/vmlinuz-6.16.0 MEM=512
KERNEL ?= images/bzImage
INITRD ?= build/initramfs.cpio.gz
MEM ?= 256
# make disk / make run-disk：64 MiB 的 ext4 镜像，已存在时不覆盖。
DISK ?= build/disk.img
DISK_MB ?= 64
# make run-net：先 sudo scripts/setup-tap.sh 创建 tap0。
TAP ?= tap0
# make netns / run-netns / netns-down（Stage 08）：sandbox netns + nftables 出口 denylist。
# ALLOW_DST="10.1.2.3 10.4.0.0/16" 可以在 denylist 上开例外（例如公司代理和 DNS）。
NETNS ?= sandbox-ns

all: bin/svmm bin/virtq-demo

ablation: $(ABLATION_VARIANTS:%=bin/ablation/%/svmm)

initramfs: build/initramfs.cpio.gz

disk: | build
	@if [ -e $(DISK) ]; then \
		echo "disk: $(DISK) exists, keeping it (rm it to recreate)"; \
	else \
		truncate -s $(DISK_MB)M $(DISK) && mkfs.ext4 -q -F $(DISK) && \
		echo "disk: created $(DISK) ($(DISK_MB) MiB ext4)"; \
	fi

build/initramfs.cpio.gz: rootfs/build-rootfs.sh | build
	sh rootfs/build-rootfs.sh $@

bin/svmm: $(OBJECTS) | bin
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

bin/virtq-demo: build/virtio/demo.o $(VIRTIO_OBJECTS) | bin
	$(CC) $(CFLAGS) $^ -o $@

bin/test_virtq: tests/test_virtq.c $(VIRTIO_SOURCES) src/virtio/virtq.h src/virtio/virtio.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_virtq.c $(VIRTIO_SOURCES) -o $@

bin/test_virtio_blk: tests/test_virtio_blk.c $(BLK_SOURCES) src/virtio/virtq.h \
                     src/virtio/mmio.h src/virtio/blk.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_virtio_blk.c $(BLK_SOURCES) -o $@

bin/test_virtio_net: tests/test_virtio_net.c $(NET_SOURCES) src/virtio/virtq.h \
                     src/virtio/mmio.h src/virtio/net.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_virtio_net.c $(NET_SOURCES) -o $@

bin/test_jail: tests/test_jail.c src/jail.c src/jail.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_jail.c src/jail.c -o $@

bin/test_serial: tests/test_serial.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_serial.c src/serial.c -o $@

bin/test_uart: tests/test_uart.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_uart.c src/serial.c -o $@

bin/test_boot: tests/test_boot.c src/boot/linux.c src/boot/linux.h src/memory.c src/memory.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_boot.c src/boot/linux.c src/memory.c -o $@

bin/test_metrics: tests/test_metrics.c src/metrics.c src/metrics.h src/vcpu.h \
                  src/serial.c src/serial.h src/boot/acpi.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_metrics.c src/metrics.c src/serial.c -o $@

bin/test_vcpu_errors: tests/test_vcpu_errors.c src/vcpu.c src/vcpu.h src/boot/acpi.h src/memory.h \
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
	@./bin/svmm run --kernel $(KERNEL) --initrd $(INITRD) --mem $(MEM)

run-disk: all initramfs disk
	@./bin/svmm run --kernel $(KERNEL) --initrd $(INITRD) --mem $(MEM) --disk $(DISK)

run-net: all initramfs
	@./bin/svmm run --kernel $(KERNEL) --initrd $(INITRD) --mem $(MEM) --tap $(TAP)

# 这两个 target 需要 root：sudo make netns。
netns:
	@NETNS=$(NETNS) TAP=$(TAP) ALLOW_DST="$(ALLOW_DST)" sh scripts/setup-netns-policy.sh

netns-down:
	@NETNS=$(NETNS) TAP=$(TAP) sh scripts/setup-netns-policy.sh --clean

# VMM 自己 setns 进 $(NETNS)，不需要 ip netns exec。
run-netns: all initramfs
	@./bin/svmm run --kernel $(KERNEL) --initrd $(INITRD) --mem $(MEM) --tap $(TAP) \
		--netns $(NETNS)

# Stage 09：jailer。run-jail 需要 root；jail-setup 只是提前检查 cgroup v2，可以不跑。
jail-setup:
	@sh scripts/setup-jail.sh

jail-clean:
	@sh scripts/setup-jail.sh --clean

run-jail: all initramfs
	@./bin/svmm run --kernel $(KERNEL) --initrd $(INITRD) --mem $(MEM) --jail

help:
	@echo "make all          build bin/svmm and bin/virtq-demo"
	@echo "make initramfs    build $(INITRD)"
	@echo "make run          boot KERNEL with the initramfs"
	@echo "make run-disk     ... plus $(DISK) as virtio-blk"
	@echo "make run-net      ... plus TAP=$(TAP) as virtio-net (sudo scripts/setup-tap.sh first)"
	@echo "sudo make netns   create netns $(NETNS) with $(TAP), veth, NAT and the egress denylist"
	@echo "make run-netns    boot with --tap $(TAP) --netns $(NETNS) (as root)"
	@echo "sudo make netns-down   remove everything 'make netns' created"
	@echo "sudo make jail-setup   check cgroup v2 and create /sys/fs/cgroup/svmm"
	@echo "sudo make run-jail     boot with --jail (cgroup, rlimit, no capabilities, seccomp)"
	@echo "sudo make jail-clean   remove cgroups left by exited VMMs"
	@echo "make test         unit + E2E tests (E2E needs BZIMAGE_PATH and root)"
	@echo "make clean        remove build/ and bin/"

run-virtq: bin/virtq-demo
	@./bin/virtq-demo

test: bin/test_metrics bin/test_vcpu_errors bin/test_virtq bin/test_virtio_blk \
      bin/test_virtio_net bin/test_jail
	./bin/test_virtq
	./bin/test_virtio_blk
	./bin/test_virtio_net
	./bin/test_jail
	python3 -m unittest -v tests/test_virtq_demo.py
	./bin/test_metrics
	./bin/test_vcpu_errors
	python3 -m unittest -v tests/test_ablation_collect.py
	sh tests/test_loader_ablations.sh
	sh tests/test_linux_boot.sh
	python3 -m unittest -v tests/test_initramfs_shell.py
	python3 -m unittest -v tests/test_sandbox_cli.py
	python3 -m unittest -v tests/test_virtio_blk.py
	python3 -m unittest -v tests/test_virtio_net.py
	python3 -m unittest -v tests/test_network_policy.py
	python3 -m unittest -v tests/test_jailer.py

clean:
	rm -rf build bin

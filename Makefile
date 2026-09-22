CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE
CPPFLAGS += -Isrc

SOURCES := src/main.c src/kvm.c src/memory.c src/vcpu.c src/serial.c src/boot/linux.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)
DEPFILES := $(OBJECTS:.o=.d)

-include $(DEPFILES)

.PHONY: all run test clean

all: bin/linux_boot

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

run: all
	@./bin/linux_boot

test:
	sh tests/test_linux_boot.sh

clean:
	rm -rf build bin

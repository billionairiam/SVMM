CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE
CPPFLAGS += -Isrc

SOURCES := src/main.c src/kvm.c src/memory.c src/vcpu.c src/serial.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)

.PHONY: all run test clean

all: bin/serial_console

bin/serial_console: $(OBJECTS) | bin
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

bin/test_serial: tests/test_serial.c src/serial.c src/serial.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_serial.c src/serial.c -o $@

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build bin:
	mkdir -p $@

run: all
	@./bin/serial_console

test:
	sh tests/test_serial_console.sh

clean:
	rm -rf build bin

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -D_GNU_SOURCE
CPPFLAGS += -Isrc

BUILD_DIR := build
BIN := $(BUILD_DIR)/stage-00-kvm-hello
SRCS := src/main.c src/kvm.c src/memory.c src/vcpu.c
OBJS := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all run test clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

run: all
	@./$(BIN)

test:
	sh tests/test_kvm_hello.sh

clean:
	rm -rf $(BUILD_DIR)

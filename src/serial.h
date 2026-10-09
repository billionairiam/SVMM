#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COM1_PORT 0x3f8u
#define COM1_PORT_REGION_SIZE 8u

struct serial {
    int out_fd;
};

void serial_init(struct serial *serial, int out_fd);
bool serial_handles_port(uint16_t port);
/* 处理一次 OUT 退出：data 中有 count 个元素，每个元素 size 字节。 */
int serial_handle_out(struct serial *serial, uint16_t port,
                      const uint8_t *data, size_t size, size_t count);

#endif

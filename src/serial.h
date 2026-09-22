#ifndef SERIAL_H
#define SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COM1_PORT 0x3f8u
#define COM1_PORT_REGION_SIZE 8u

struct serial {
    int out_fd;
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t fcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
};

void serial_init(struct serial *serial, int out_fd);
bool serial_handles_port(uint16_t port);
int serial_handle_out(struct serial *serial, uint16_t port,
                      const uint8_t *data, size_t size);
int serial_handle_in(struct serial *serial, uint16_t port, uint8_t *value);

#endif

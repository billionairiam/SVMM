#include "serial.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

void serial_init(struct serial *serial, int out_fd)
{
    memset(serial, 0, sizeof(*serial));
    serial->out_fd = out_fd;
}

bool serial_handles_port(uint16_t port)
{
    return port >= COM1_PORT && port < COM1_PORT + COM1_PORT_REGION_SIZE;
}

int serial_handle_out(struct serial *serial, uint16_t port,
                      const uint8_t *data, size_t size)
{
    if (!serial_handles_port(port)) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 0;

    switch (port - COM1_PORT) {
    case 0:
        if (serial->lcr & 0x80) {
            serial->dll = data[0];
            return 0;
        }
        break;
    case 1:
        if (serial->lcr & 0x80)
            serial->dlm = data[0];
        else
            serial->ier = data[0] & 0x0f;
        return 0;
    case 2:
        serial->fcr = data[0] & 0x01;
        return 0;
    case 3:
        serial->lcr = data[0];
        return 0;
    case 4:
        serial->mcr = data[0];
        return 0;
    case 7:
        serial->scr = data[0];
        return 0;
    default:
        return 0;
    }

    size_t written = 0;
    while (written < size) {
        ssize_t n = write(serial->out_fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

int serial_handle_in(struct serial *serial, uint16_t port, uint8_t *value)
{
    if (!serial_handles_port(port)) {
        errno = EINVAL;
        return -1;
    }
    switch (port - COM1_PORT) {
    case 0:
        *value = (serial->lcr & 0x80) ? serial->dll : 0;
        break;
    case 1:
        *value = (serial->lcr & 0x80) ? serial->dlm : serial->ier;
        break;
    case 2:
        *value = (serial->fcr ? 0xc0 : 0) | 0x01;
        break;
    case 3:
        *value = serial->lcr;
        break;
    case 4:
        *value = serial->mcr;
        break;
    case 5:
        *value = 0x60; /* THR empty, transmitter empty */
        break;
    case 6:
        if (serial->mcr & 0x10) {
            *value = ((serial->mcr & 0x02) ? 0x10 : 0) |
                     ((serial->mcr & 0x01) ? 0x20 : 0) |
                     ((serial->mcr & 0x04) ? 0x40 : 0) |
                     ((serial->mcr & 0x08) ? 0x80 : 0);
        } else {
            *value = 0xb0; /* CTS, DSR and DCD asserted */
        }
        break;
    case 7:
        *value = serial->scr;
        break;
    default:
        *value = 0;
        break;
    }
    return 0;
}

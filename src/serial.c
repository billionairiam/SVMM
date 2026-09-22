#include "serial.h"

#include <errno.h>
#include <unistd.h>

void serial_init(struct serial *serial, int out_fd)
{
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
    if (port != COM1_PORT)
        return 0;

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

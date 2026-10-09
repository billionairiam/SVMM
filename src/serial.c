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

static int write_all(int fd, const uint8_t *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, data + written, size - written);
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

int serial_handle_out(struct serial *serial, uint16_t port,
                      const uint8_t *data, size_t size, size_t count)
{
    if (size == 0 || !serial_handles_port(port) ||
        size > COM1_PORT + COM1_PORT_REGION_SIZE - port) {
        errno = EINVAL;
        return -1;
    }
    /*
     * 16550 寄存器都是 8 位宽：宽度为 size 的访问会把第 i 个字节送到 port + i。
     * 由于 port >= COM1_PORT，只有从数据寄存器起始的访问会命中它，
     * 且只有每个元素的第 0 个字节是要输出的数据，其余字节落到 IER 等寄存器上被忽略。
     */
    if (port != COM1_PORT)
        return 0;
    if (size == 1)
        return write_all(serial->out_fd, data, count);

    uint8_t buffer[256];
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        buffer[used++] = data[i * size];
        if (used == sizeof(buffer) || i + 1 == count) {
            if (write_all(serial->out_fd, buffer, used) < 0)
                return -1;
            used = 0;
        }
    }
    return 0;
}

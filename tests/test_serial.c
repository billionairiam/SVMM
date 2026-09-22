#include "serial.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);

    struct serial serial;
    serial_init(&serial, pipefd[1]);
    assert(serial_handles_port(0x3f8));
    assert(serial_handles_port(0x3ff));
    assert(!serial_handles_port(0x3f7));
    assert(!serial_handles_port(0x400));

    const uint8_t message[] = { 'h', 'i', '\n' };
    assert(serial_handle_out(&serial, 0x3f8, message, sizeof(message)) == 0);
    assert(serial_handle_out(&serial, 0x3f9, message, sizeof(message)) == 0);
    assert(serial_handle_out(&serial, 0x400, message, sizeof(message)) == -1);
    close(pipefd[1]);

    uint8_t result[8] = { 0 };
    assert(read(pipefd[0], result, sizeof(result)) == (ssize_t)sizeof(message));
    assert(memcmp(result, message, sizeof(message)) == 0);
    close(pipefd[0]);

    serial_init(&serial, -1);
    assert(serial_handle_out(&serial, 0x3f8, message, sizeof(message)) == -1);
    assert(errno == EBADF);
    return 0;
}

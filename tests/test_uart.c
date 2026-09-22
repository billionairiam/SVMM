#include "serial.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    struct serial serial;
    serial_init(&serial, pipefd[1]);

    uint8_t value;
    assert(serial_handle_in(&serial, 0x3fd, &value) == 0);
    assert(value & 0x60);             /* transmitter is ready */
    assert(serial_handle_in(&serial, 0x3fa, &value) == 0);
    assert(value & 0x01);             /* no interrupt pending */

    const uint8_t dlab = 0x80;
    const uint8_t divisor = 0x0c;
    const uint8_t normal = 0x03;
    const uint8_t character = 'X';
    assert(serial_handle_out(&serial, 0x3fb, &dlab, 1) == 0);
    assert(serial_handle_out(&serial, 0x3f8, &divisor, 1) == 0);
    assert(serial_handle_in(&serial, 0x3f8, &value) == 0);
    assert(value == divisor);
    assert(serial_handle_out(&serial, 0x3fb, &normal, 1) == 0);
    assert(serial_handle_out(&serial, 0x3f8, &character, 1) == 0);
    assert(serial_handle_out(&serial, 0x3ff, &divisor, 1) == 0);
    assert(serial_handle_in(&serial, 0x3ff, &value) == 0);
    assert(value == divisor);
    close(pipefd[1]);
    assert(read(pipefd[0], &value, 1) == 1);
    assert(value == character);
    close(pipefd[0]);
    return 0;
}

#include "check.h"
#include "serial.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    struct serial serial;
    serial_init(&serial, pipefd[1]);

    uint8_t value;
    CHECK(serial_handle_in(&serial, 0x3fd, &value) == 0);
    CHECK(value & 0x60);             /* 发送器空闲 */
    CHECK(serial_handle_in(&serial, 0x3fa, &value) == 0);
    CHECK(value & 0x01);             /* 没有待处理中断 */

    const uint8_t dlab = 0x80;
    const uint8_t divisor = 0x0c;
    const uint8_t normal = 0x03;
    const uint8_t character = 'X';
    CHECK(serial_handle_out(&serial, 0x3fb, dlab) == 0);
    CHECK(serial_handle_out(&serial, 0x3f8, divisor) == 0);
    CHECK(serial_handle_in(&serial, 0x3f8, &value) == 0);
    CHECK(value == divisor);
    CHECK(serial_handle_out(&serial, 0x3fb, normal) == 0);
    CHECK(serial_handle_out(&serial, 0x3f8, character) == 0);
    CHECK(serial_handle_out(&serial, 0x3ff, divisor) == 0);
    CHECK(serial_handle_in(&serial, 0x3ff, &value) == 0);
    CHECK(value == divisor);
    close(pipefd[1]);
    CHECK(read(pipefd[0], &value, 1) == 1);
    CHECK(value == character);
    close(pipefd[0]);
    return 0;
}

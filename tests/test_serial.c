#include "check.h"
#include "serial.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/*
 * serial_handle_out() 按单字节访问 UART 寄存器。可能出错的地方：
 *   1. 非 COM1 端口被当成串口处理，或没有报告 EINVAL；
 *   2. 写 THR 以外的寄存器（如 IER）也把字节输出到宿主机；
 *   3. DLAB=1 时写偏移 0 是除数锁存器，却被当成字符输出；
 *   4. out_fd 写失败时吞掉错误。
 */
int main(void)
{
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);

    struct serial serial;
    serial_init(&serial, pipefd[1]);
    CHECK(serial_handles_port(0x3f8));
    CHECK(serial_handles_port(0x3ff));
    CHECK(!serial_handles_port(0x3f7));
    CHECK(!serial_handles_port(0x400));

    CHECK(serial_handle_out(&serial, 0x3f8, 'h') == 0);
    CHECK(serial_handle_out(&serial, 0x3f9, 'X') == 0);
    CHECK(serial_handle_out(&serial, 0x3fb, 0x80) == 0);
    CHECK(serial_handle_out(&serial, 0x3f8, 'Y') == 0);
    CHECK(serial_handle_out(&serial, 0x3fb, 0x03) == 0);
    CHECK(serial_handle_out(&serial, 0x3f8, 'i') == 0);
    errno = 0;
    CHECK(serial_handle_out(&serial, 0x400, 'Z') == -1);
    CHECK(errno == EINVAL);
    CHECK(close(pipefd[1]) == 0);

    char result[8] = { 0 };
    CHECK(read(pipefd[0], result, sizeof(result)) == 2);
    CHECK(memcmp(result, "hi", 2) == 0);
    CHECK(close(pipefd[0]) == 0);

    serial_init(&serial, -1);
    CHECK(serial_handle_out(&serial, 0x3f8, 'h') == -1);
    CHECK(errno == EBADF);
    return 0;
}

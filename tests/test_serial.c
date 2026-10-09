#include "serial.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 不用 assert：-DNDEBUG 下它会连同副作用一起消失。 */
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,         \
                    __LINE__, #cond);                                      \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/* 执行 out 并返回写入 out_fd 的全部字节。 */
static size_t capture(uint16_t port, const uint8_t *data, size_t size,
                      size_t count, int *ret, uint8_t *out, size_t out_cap)
{
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    struct serial serial;
    serial_init(&serial, pipefd[1]);
    *ret = serial_handle_out(&serial, port, data, size, count);
    CHECK(close(pipefd[1]) == 0);

    size_t total = 0;
    for (;;) {
        ssize_t n = read(pipefd[0], out + total, out_cap - total);
        CHECK(n >= 0);
        if (n == 0)
            break;
        total += (size_t)n;
        CHECK(total < out_cap);
    }
    CHECK(close(pipefd[0]) == 0);
    return total;
}

int main(void)
{
    uint8_t out[64];
    int ret;
    size_t n;

    /* 数据寄存器的单字节写原样输出。 */
    const uint8_t hi[] = { 'h', 'i', '\n' };
    n = capture(0x3f8, hi, 1, sizeof(hi), &ret, out, sizeof(out));
    CHECK(ret == 0 && n == sizeof(hi) && memcmp(out, hi, n) == 0);

    /* IER..SR 的写被静默忽略，不能漏到输出里。 */
    for (uint16_t port = 0x3f9; port <= 0x3ff; port++) {
        n = capture(port, hi, 1, 1, &ret, out, sizeof(out));
        CHECK(ret == 0 && n == 0);
    }

    /* out dx, ax：低字节进数据寄存器，高字节落到 IER，只输出低字节。 */
    const uint8_t words[] = { 'o', 0xaa, 'k', 0xbb };
    n = capture(0x3f8, words, 2, 2, &ret, out, sizeof(out));
    CHECK(ret == 0 && n == 2 && out[0] == 'o' && out[1] == 'k');

    /* out dx, eax 同理。 */
    const uint8_t dword[] = { 'z', 1, 2, 3 };
    n = capture(0x3f8, dword, 4, 1, &ret, out, sizeof(out));
    CHECK(ret == 0 && n == 1 && out[0] == 'z');

    /* 不属于 COM1 的端口，或跨出 COM1 末尾的访问必须报错且不输出。 */
    n = capture(0x3f7, hi, 1, 1, &ret, out, sizeof(out));
    CHECK(ret == -1 && errno == EINVAL && n == 0);
    n = capture(0x400, hi, 1, 1, &ret, out, sizeof(out));
    CHECK(ret == -1 && errno == EINVAL && n == 0);
    n = capture(0x3fe, dword, 4, 1, &ret, out, sizeof(out));
    CHECK(ret == -1 && errno == EINVAL && n == 0);

    /* 写失败要向上报告 errno。 */
    struct serial bad;
    serial_init(&bad, -1);
    CHECK(serial_handle_out(&bad, 0x3f8, hi, 1, sizeof(hi)) == -1);
    CHECK(errno == EBADF);

    puts("PASS: serial unit");
    return 0;
}

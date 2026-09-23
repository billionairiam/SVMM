#include "console.h"

#include "serial.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 宿主机控制台：后台线程读取 in_fd（通常是 stdin），把字节投递给 COM1
 * 的接收队列，客户机的 8250 驱动收到接收中断后读出，shell 因此可以交互。
 *
 * in_fd 是终端时切到 raw 模式：回显、行编辑、Ctrl-C 等全部交给客户机的
 * tty 层处理。raw 模式下宿主机收不到 SIGINT，所以用 Ctrl-A x 退出 VMM，
 * Ctrl-A Ctrl-A 发送一个字面的 Ctrl-A。in_fd 是管道或文件时原样转发，
 * 读到 EOF 后线程结束，客户机继续运行。
 */

#define CONSOLE_ESCAPE 0x01 /* Ctrl-A */

static void *console_thread(void *opaque)
{
    struct console *console = opaque;
    bool escape = false;
    bool quit = false;
    uint8_t input[256];
    uint8_t output[sizeof(input)];
    while (!quit) {
        ssize_t n = read(console->in_fd, input, sizeof(input));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size_t count = 0;
        for (ssize_t i = 0; i < n && !quit; ++i) {
            uint8_t byte = input[i];
            if (!console->raw) {
                output[count++] = byte;
            } else if (escape) {
                escape = false;
                if (byte == 'x' || byte == 'X')
                    quit = true;
                else if (byte == CONSOLE_ESCAPE)
                    output[count++] = byte;
            } else if (byte == CONSOLE_ESCAPE) {
                escape = true;
            } else {
                output[count++] = byte;
            }
        }
        /* 等待接收队列空间时持有串口锁，这段期间不能被取消，否则锁不会释放。 */
        int old_state;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
        int result = count ? serial_receive(console->serial, output, count) : 0;
        pthread_setcancelstate(old_state, NULL);
        if (result < 0)
            return NULL;
    }
    if (quit)
        console->quit(console->quit_opaque);
    return NULL;
}

int console_start(struct console *console, struct serial *serial, int in_fd,
                  console_quit_fn quit, void *quit_opaque)
{
    memset(console, 0, sizeof(*console));
    console->serial = serial;
    console->in_fd = in_fd;
    console->quit = quit;
    console->quit_opaque = quit_opaque;

    if (isatty(in_fd) && tcgetattr(in_fd, &console->saved) == 0) {
        struct termios raw = console->saved;
        cfmakeraw(&raw);
        if (tcsetattr(in_fd, TCSANOW, &raw) == 0) {
            console->raw = true;
            fprintf(stderr, "INFO: serial console attached; press Ctrl-A x to quit\r\n");
        }
    }

    /* 让 SIGINT/SIGTERM 等信号只送到 vCPU 所在的主线程。 */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int error = pthread_create(&console->thread, NULL, console_thread, console);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (error) {
        errno = error;
        perror("pthread_create console");
        console_stop(console);
        return -1;
    }
    console->started = true;
    return 0;
}

void console_stop(struct console *console)
{
    if (console->started) {
        serial_close_input(console->serial);
        pthread_cancel(console->thread);
        pthread_join(console->thread, NULL);
        console->started = false;
    }
    if (console->raw) {
        tcsetattr(console->in_fd, TCSANOW, &console->saved);
        console->raw = false;
    }
}

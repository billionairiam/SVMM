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

/*
 * 控制台线程主循环：read(in_fd) → 处理 Ctrl-A 转义 → serial_receive()。
 *
 * 结束条件：
 *   - 用户按下 Ctrl-A x：跳出循环后调用 quit 回调，请求 VMM 退出；
 *   - 读到 EOF 或读错误（例如管道输入写完了）：线程悄悄结束，客户机照常
 *     运行，只是不再有输入；
 *   - 串口输入已关闭（VMM 正在退出）：serial_receive() 返回 -1；
 *   - 被 console_stop() 取消：阻塞在 read() 时，read() 是取消点。
 */
static void *console_thread(void *opaque)
{
    struct console *console = opaque;
    /* 上一个字节是 Ctrl-A，正在等待命令字符。跨越多次 read() 保持。 */
    bool escape = false;
    /* 收到 Ctrl-A x，处理完本批字节后退出循环。 */
    bool quit = false;
    uint8_t input[256];
    /* 去掉转义序列后要送给客户机的字节，不会比 input 多。 */
    uint8_t output[sizeof(input)];
    while (!quit) {
        ssize_t n = read(console->in_fd, input, sizeof(input));
        /* 被信号打断不是错误，重新读即可。 */
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        /*
         * 过滤转义序列。只有终端（raw 模式）才解释 Ctrl-A，管道和文件
         * 输入原样转发，这样二进制数据里的 0x01 不会被吞掉。
         *   Ctrl-A x / X     退出 VMM，同一批中其后的字节被丢弃
         *   Ctrl-A Ctrl-A    向客户机发送一个 Ctrl-A
         *   Ctrl-A 其他字符  两个字节都丢弃
         */
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
        /*
         * 投递给串口。接收队列满时 serial_receive() 会在条件变量上等待，
         * 而 pthread_cond_wait() 是取消点：如果在那里被取消，线程会带着
         * 串口锁退出，vCPU 线程下次访问串口就会永远卡住。所以投递期间
         * 暂时禁止取消；console_stop() 改用 serial_close_input() 唤醒这里。
         */
        int old_state;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
        int result = count ? serial_receive(console->serial, output, count) : 0;
        pthread_setcancelstate(old_state, NULL);
        if (result < 0)
            return NULL;
    }
    /*
     * 在线程即将结束时才通知退出。quit 回调会让 vCPU 停下，主线程随后
     * 调用 console_stop() 回收本线程，此时本线程已经（或马上）返回。
     */
    if (quit)
        console->quit(console->quit_opaque);
    return NULL;
}

/*
 * 初始化 console，把终端切到 raw 模式（如果 in_fd 是终端），再创建
 * 控制台线程。线程创建失败时恢复终端设置并返回 -1。
 */
int console_start(struct console *console, struct serial *serial, int in_fd,
                  console_quit_fn quit, void *quit_opaque)
{
    memset(console, 0, sizeof(*console));
    console->serial = serial;
    console->in_fd = in_fd;
    console->quit = quit;
    console->quit_opaque = quit_opaque;

    /*
     * 终端默认是“规范模式”：宿主机的 tty 层自己做回显、按行缓冲、处理
     * 退格，并把 Ctrl-C 变成发给 VMM 的 SIGINT。客户机里也有一个 tty 层
     * 在做同样的事，两层叠加会出现双重回显、按回车才能发送、Ctrl-C
     * 杀掉 VMM 而不是客户机里的程序等问题。
     *
     * cfmakeraw() 关闭宿主机这一层的所有加工，让每个按键原样、立刻送进
     * 客户机。先用 tcgetattr() 保存原设置，退出时再恢复，否则 VMM 退出后
     * 用户的 shell 会停留在 raw 模式（看不到输入，可用 `stty sane` 修复）。
     */
    if (isatty(in_fd) && tcgetattr(in_fd, &console->saved) == 0) {
        struct termios raw = console->saved;
        cfmakeraw(&raw);
        if (tcsetattr(in_fd, TCSANOW, &raw) == 0) {
            console->raw = true;
            /* raw 模式也关闭了输出加工（OPOST），"\n" 不再自动加回车。 */
            fprintf(stderr, "INFO: serial console attached; press Ctrl-A x to quit\r\n");
        }
    }

    /*
     * 新线程继承创建者的信号掩码。创建前暂时屏蔽所有信号，控制台线程
     * 就永远不会接收信号；发给进程的 SIGINT/SIGTERM 等只能送到主线程，
     * 也就是运行 KVM_RUN 的 vCPU 线程，正好打断 KVM_RUN 让它检查停止请求。
     * 创建完再恢复主线程自己的掩码。
     */
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

/*
 * 控制台线程可能阻塞在两个地方，需要分别唤醒：
 *   - 阻塞在 serial_receive() 等待队列空间（此时禁止取消）：
 *     serial_close_input() 让它返回错误，线程自行结束；
 *   - 阻塞在 read(in_fd) 等待用户输入：read() 是取消点，
 *     pthread_cancel() 让线程在这里结束。
 * pthread_join() 等线程真正结束后，才能安全恢复终端设置。
 */
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

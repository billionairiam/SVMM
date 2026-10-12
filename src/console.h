#ifndef CONSOLE_H
#define CONSOLE_H

#include <pthread.h>
#include <stdbool.h>
#include <termios.h>

struct serial;

/* 用户按下 Ctrl-A x 时，控制台线程调用这个回调请求 VMM 退出。 */
typedef void (*console_quit_fn)(void *opaque);

/*
 * 宿主机控制台：把宿主机的键盘输入（或管道输入）送进客户机的串口。
 *
 * 数据流向：
 *   键盘 → in_fd → 控制台线程 → serial_receive() → COM1 接收队列
 *        → 接收中断（IRQ 4）→ 客户机 8250 驱动读 RBR → 客户机 shell
 * 反方向（客户机输出 → 宿主机屏幕）不经过这里，由 vCPU 线程处理 THR
 * 写入时直接写到 stdout。
 */
struct console {
    /* 输入要投递到的串口设备模型。 */
    struct serial *serial;
    /* 读取输入的宿主机文件描述符，通常是 STDIN_FILENO。 */
    int in_fd;
    /*
     * in_fd 是否已被切换到终端 raw 模式。只有 in_fd 是终端时才会切换；
     * 为 true 时 console_stop() 需要用 saved 恢复终端设置，Ctrl-A 转义
     * 也只在这种模式下生效。
     */
    bool raw;
    /* 控制台线程是否已创建成功；为 true 时 console_stop() 需要结束并回收它。 */
    bool started;
    /* 切换到 raw 模式之前的终端设置，退出时原样恢复。 */
    struct termios saved;
    /* 控制台线程：循环 read(in_fd) 并把字节投递给串口。 */
    pthread_t thread;
    /* Ctrl-A x 时调用 quit(quit_opaque)；main.c 中它会让 vCPU 停止运行。 */
    console_quit_fn quit;
    void *quit_opaque;
};

/*
 * 启动控制台：必要时把 in_fd 切到 raw 模式，并创建控制台线程。
 * 成功返回 0；失败返回 -1，此时终端设置已经恢复。
 */
int console_start(struct console *console, struct serial *serial, int in_fd,
                  console_quit_fn quit, void *quit_opaque);
/*
 * 结束控制台线程并恢复终端设置。可以重复调用；对未启动或启动失败的
 * console 调用也是安全的（要求结构体已清零或经过 console_start()）。
 */
void console_stop(struct console *console);

#endif

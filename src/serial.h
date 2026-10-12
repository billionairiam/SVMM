#ifndef SERIAL_H
#define SERIAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COM1_PORT 0x3f8u
#define COM1_PORT_REGION_SIZE 8u
#define COM1_IRQ 4u
#define SERIAL_RX_CAPACITY 4096u

typedef void (*serial_irq_fn)(void *opaque, int level);

/*
 * 一个 16550A UART（COM1）的软件模型。字段分四组：
 *   1. 输出目标：客户机发出的字节写到哪里；
 *   2. 客户机可见的寄存器：驱动通过 IN/OUT 指令读写的那些 8 位寄存器；
 *   3. 中断状态：决定 IIR 的内容以及 IRQ 4 的电平；
 *   4. 接收队列和线程同步：宿主机输入线程与 vCPU 线程共享的数据。
 * 寄存器的端口偏移和读写含义见 serial.c 开头的表格。
 */
struct serial {
    /* ---- 1. 输出目标 ---- */

    /*
     * 客户机写 THR（发送保持寄存器）的字节最终写到这个宿主机文件描述符，
     * 通常是 stdout。环回模式（MCR.LOOP）下字节不会写到这里。
     */
    int out_fd;

    /* ---- 2. 客户机可见的寄存器 ---- */

    /*
     * IER，中断使能寄存器（偏移 1，DLAB=0）。只保存低 4 位：
     *   bit 0 RDI：接收到数据时产生中断
     *   bit 1 THRI：发送保持寄存器空时产生中断
     *   bit 2/3：线路状态、modem 状态中断，只保存供读回，本模型不会产生
     */
    uint8_t ier;
    /*
     * LCR，线路控制寄存器（偏移 3）。bit 7 是 DLAB（除数锁存器访问位）：
     * 置 1 时偏移 0/1 访问的是 DLL/DLM，而不是 THR/RBR 和 IER。其余位
     * （数据位、停止位、校验）只保存供驱动读回，不影响输出。
     */
    uint8_t lcr;
    /*
     * MCR，modem 控制寄存器（偏移 4）：
     *   bit 0 DTR、bit 1 RTS、bit 2 OUT1：只在环回模式下映射到 MSR
     *   bit 3 OUT2：PC 上 UART 的中断输出要经过 OUT2 才能到达中断控制器，
     *               清零时即使有中断条件 IRQ 4 也保持低电平
     *   bit 4 LOOP：环回模式，发送的字节回到接收队列，不输出到宿主机
     */
    uint8_t mcr;
    /*
     * FCR，FIFO 控制寄存器（偏移 2 的写方向）。真实硬件上它只能写、
     * 不能读；这里只保留 bit 0（FIFO 使能），用来决定读 IIR 时
     * bit 7:6 是否报告 11（Linux 据此识别出 16550A）。
     * “清空接收 FIFO”等其他位在写入时立即执行，不需要保存。
     */
    uint8_t fcr;
    /*
     * SCR，暂存寄存器（偏移 7）。没有任何硬件功能，驱动写入一个值再读回，
     * 用来判断这个端口上是否真的有 UART。
     */
    uint8_t scr;
    /*
     * DLL/DLM：除数锁存器的低/高字节（DLAB=1 时的偏移 0/1）。真实硬件上
     * 波特率 = 115200 / (DLM << 8 | DLL)；这里发送瞬间完成，与波特率无关，
     * 所以只保存供驱动读回。
     */
    uint8_t dll;
    uint8_t dlm;

    /* ---- 3. 中断状态 ---- */

    /*
     * THR 空中断是否在等待驱动处理。THR 空不是持续的条件，而是一个
     * “事件”：每次写 THR（字符立即发完，THR 又空了）或 IER.THRI 从 0 变 1
     * 时置位；驱动读 IIR 读到 THR 空中断时清除。只有置位且 IER.THRI=1 时，
     * IIR 才会报告这个中断。
     */
    bool thre_pending;
    /*
     * 上一次通过 set_irq 报告的 INTR 输出电平。每次寄存器访问后重新计算
     * 电平，只在它变化时才调用 set_irq：ISA IRQ 是边沿触发的，
     * 0 → 1 的跳变才代表一次新的中断请求。
     */
    bool irq_level;

    /* ---- 4. 接收队列和线程同步 ---- */

    /*
     * 输入已关闭（VMM 退出时由 serial_close_input() 设置）。之后
     * serial_receive() 不再阻塞，直接返回错误，让输入线程能结束。
     */
    bool rx_closed;
    /*
     * 接收队列：宿主机输入等待客户机从 RBR 读出的字节，按环形缓冲区组织。
     * 真实 16550A 的接收 FIFO 只有 16 字节，这里做到 4 KiB，让粘贴或管道
     * 输入时宿主机侧有更大的缓冲；环回模式仍按 16 字节限制。
     */
    uint8_t rx[SERIAL_RX_CAPACITY];
    /* 最早进入队列、下一个要被读出的字节在 rx 中的下标。 */
    size_t rx_head;
    /*
     * 队列中的字节数。下一个空位是 (rx_head + rx_count) % SERIAL_RX_CAPACITY。
     * 非 0 时 LSR.DR=1，并且（IER.RDI=1 时）产生接收中断。
     */
    size_t rx_count;
    /*
     * 中断线回调：电平变化时调用 set_irq(irq_opaque, level)。
     * main.c 中它调用 KVM_IRQ_LINE，把电平送到 KVM 模拟的中断控制器的 IRQ 4。
     */
    serial_irq_fn set_irq;
    /*
     * 原样传给 set_irq 的第一个参数，串口自己从不解读它。
     *
     * 为什么需要它：串口只知道“中断线电平变了”，但要真正把电平送进
     * 客户机，回调还得知道往哪个 VM 发，也就是 KVM 的 VM fd。串口不应该
     * 依赖 KVM（这样换一种中断控制器时串口代码不用改），所以由调用者
     * 把回调需要的上下文用 void * 交给串口保管。
     * 这是 C 语言里模拟“闭包”的常见写法，相当于把函数和它的数据绑在一起。
     *
     * 在本项目中：main.c 调用
     *   serial_set_irq_handler(&serial, serial_irq, &kvm);
     * 所以 irq_opaque 指向 main() 里的 struct kvm_context，serial_irq()
     * 再把它传给 kvm_set_irq_line(opaque, COM1_IRQ, level)。
     */
    void *irq_opaque;
    /*
     * 保护本结构体的所有可变字段。vCPU 线程（客户机访问寄存器）和控制台
     * 输入线程（serial_receive() 投递字节）会同时访问它们。
     */
    pthread_mutex_t lock;
    /*
     * 接收队列有空位的条件变量。队列满时 serial_receive() 在这里等待；
     * 客户机读走字节、FCR 清空 FIFO 或输入关闭时被唤醒。这样输入方被
     * 阻塞住（背压），而不是丢掉字节。
     */
    pthread_cond_t rx_space;
};

void serial_init(struct serial *serial, int out_fd);
void serial_destroy(struct serial *serial);
void serial_set_irq_handler(struct serial *serial, serial_irq_fn set_irq,
                            void *opaque);
bool serial_handles_port(uint16_t port);
int serial_handle_out(struct serial *serial, uint16_t port,
                      const uint8_t *data, size_t size);
int serial_handle_in(struct serial *serial, uint16_t port, uint8_t *value);
int serial_receive(struct serial *serial, const uint8_t *data, size_t size);
void serial_close_input(struct serial *serial);

#endif

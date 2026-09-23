#include "serial.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/*
 * 最小化的 16550A UART 模型，只模拟 COM1（I/O 端口 0x3f8–0x3ff，IRQ 4），
 * 让 Linux 的 8250 串口驱动既能输出控制台，也能接收宿主机输入。
 *
 *   - 客户机写 THR 的字节原样写到 out_fd（通常是宿主机 stdout）；
 *   - 宿主机输入经 serial_receive() 进入接收队列，客户机从 RBR 读出；
 *   - 发送永远立即完成：LSR 恒报告发送缓冲区和移位寄存器为空；
 *   - 中断：IER 使能的“接收数据可用”和“THR 空”两种中断，经 IIR 报告，
 *     并通过 set_irq 回调驱动 IRQ 4 的电平；
 *   - MCR.LOOP 回环模式下发送的字节回到接收 FIFO，不输出到宿主机；
 *   - 波特率、数据位等配置只保存下来供读回，不影响输出。
 *
 * 8 个端口相对 COM1_PORT 的偏移及含义（DLAB 为 LCR bit 7）：
 *
 *   偏移  DLAB  读                       写
 *   0     0     RBR 接收缓冲             THR 发送缓冲
 *   0     1     DLL 除数低字节           DLL
 *   1     0     IER 中断使能             IER
 *   1     1     DLM 除数高字节           DLM
 *   2     -     IIR 中断标识             FCR FIFO 控制
 *   3     -     LCR 线路控制             LCR
 *   4     -     MCR modem 控制           MCR
 *   5     -     LSR 线路状态             （只读，写入忽略）
 *   6     -     MSR modem 状态           （只读，写入忽略）
 *   7     -     SCR 暂存寄存器           SCR
 *
 * 所有 UART 寄存器都是 8 位，因此每次访问只使用第一个字节。vCPU 线程
 * 访问寄存器、输入线程投递数据都经过 lock 串行化。
 */

#define IER_RDI 0x01u   /* 接收数据可用中断使能 */
#define IER_THRI 0x02u  /* THR 空中断使能 */
#define IIR_NO_INT 0x01u
#define IIR_THRI 0x02u
#define IIR_RDI 0x04u
#define IIR_FIFO 0xc0u
#define FCR_ENABLE 0x01u
#define FCR_CLEAR_RCVR 0x02u
#define MCR_OUT2 0x08u
#define MCR_LOOP 0x10u
#define UART_FIFO_SIZE 16u
#define LSR_DR 0x01u
#define LSR_THRE_TEMT 0x60u

/* 清空所有寄存器（相当于上电复位），并记录输出目标文件描述符。 */
void serial_init(struct serial *serial, int out_fd)
{
    memset(serial, 0, sizeof(*serial));
    serial->out_fd = out_fd;
    pthread_mutex_init(&serial->lock, NULL);
    pthread_cond_init(&serial->rx_space, NULL);
}

void serial_destroy(struct serial *serial)
{
    pthread_cond_destroy(&serial->rx_space);
    pthread_mutex_destroy(&serial->lock);
}

/* 注册中断线回调：level 为 1 表示 UART 的 INTR 输出有效。 */
void serial_set_irq_handler(struct serial *serial, serial_irq_fn set_irq,
                            void *opaque)
{
    pthread_mutex_lock(&serial->lock);
    serial->set_irq = set_irq;
    serial->irq_opaque = opaque;
    pthread_mutex_unlock(&serial->lock);
}

/*
 * 按 16550 优先级计算 IIR：接收数据可用（0x04）高于 THR 空（0x02），
 * 都没有时 bit 0 = 1 表示无中断。启用 FIFO 后 bit 7:6 = 11，驱动据此
 * 识别出 16550A。
 */
static uint8_t serial_iir_locked(const struct serial *serial)
{
    uint8_t fifo = (serial->fcr & FCR_ENABLE) ? IIR_FIFO : 0;
    if ((serial->ier & IER_RDI) && serial->rx_count)
        return fifo | IIR_RDI;
    if ((serial->ier & IER_THRI) && serial->thre_pending)
        return fifo | IIR_THRI;
    return fifo | IIR_NO_INT;
}

/*
 * PC 上 UART 的 INTR 引脚要经过 MCR.OUT2 才连到中断控制器，Linux 驱动
 * 使用中断时会置位 OUT2。ISA IRQ 是边沿触发的，所以只在电平变化时通知，
 * 由 0 到 1 的跳变就是一次新的中断请求。
 */
static void serial_update_irq_locked(struct serial *serial)
{
    bool level = !(serial_iir_locked(serial) & IIR_NO_INT) &&
                 (serial->mcr & MCR_OUT2);
    if (level == serial->irq_level)
        return;
    serial->irq_level = level;
    if (serial->set_irq)
        serial->set_irq(serial->irq_opaque, level);
}

static void serial_clear_rx_locked(struct serial *serial)
{
    serial->rx_head = 0;
    serial->rx_count = 0;
    pthread_cond_broadcast(&serial->rx_space);
}

/*
 * 宿主机输入：把 data 追加到接收队列并按需触发接收中断。队列满时阻塞，
 * 等客户机读走数据后继续，因此管道输入不会丢字节。输入已关闭时返回 -1。
 */
int serial_receive(struct serial *serial, const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&serial->lock);
    size_t done = 0;
    while (done < size) {
        while (serial->rx_count == SERIAL_RX_CAPACITY && !serial->rx_closed)
            pthread_cond_wait(&serial->rx_space, &serial->lock);
        if (serial->rx_closed) {
            pthread_mutex_unlock(&serial->lock);
            errno = EPIPE;
            return -1;
        }
        while (done < size && serial->rx_count < SERIAL_RX_CAPACITY) {
            size_t tail = (serial->rx_head + serial->rx_count) % SERIAL_RX_CAPACITY;
            serial->rx[tail] = data[done++];
            ++serial->rx_count;
        }
        serial_update_irq_locked(serial);
    }
    pthread_mutex_unlock(&serial->lock);
    return 0;
}

/* 唤醒并拒绝之后的 serial_receive()，用于 VMM 退出时让输入线程结束。 */
void serial_close_input(struct serial *serial)
{
    pthread_mutex_lock(&serial->lock);
    serial->rx_closed = true;
    pthread_cond_broadcast(&serial->rx_space);
    pthread_mutex_unlock(&serial->lock);
}

/* 判断端口是否落在 COM1 的 8 个寄存器范围 [0x3f8, 0x400) 内。 */
bool serial_handles_port(uint16_t port)
{
    return port >= COM1_PORT && port < COM1_PORT + COM1_PORT_REGION_SIZE;
}

/*
 * 处理客户机对 COM1 的 OUT 指令。data/size 来自 KVM_EXIT_IO 的数据缓冲区：
 * 寄存器写入只看 data[0]；THR 写入则把全部 size 个字节依次写出，
 * 这样 REP OUTSB 一次带出的多个字符也能完整输出。
 *
 * 成功返回 0；端口不属于 COM1 时返回 -1 且 errno = EINVAL；
 * 写 out_fd 失败时返回 -1 并保留 write() 设置的 errno。
 */
int serial_handle_out(struct serial *serial, uint16_t port,
                      const uint8_t *data, size_t size)
{
    if (!serial_handles_port(port)) {
        errno = EINVAL;
        return -1;
    }
    if (size == 0)
        return 0;

    bool transmit = false;
    pthread_mutex_lock(&serial->lock);
    switch (port - COM1_PORT) {
    case 0:
        if (serial->lcr & 0x80) {
            /* DLAB=1 时偏移 0 是除数锁存器低字节，只保存不输出。 */
            serial->dll = data[0];
        } else {
            /*
             * DLAB=0 时是 THR。字符“立即发送”，THR 马上重新变空，
             * 所以再次挂起 THR 空中断，驱动收到中断后继续写下一批。
             *
             * MCR.LOOP 置位时发送端在芯片内部接回接收端，字符不会出现在
             * 线路上。Linux 用它测 FIFO 深度（写 256 字节再数能读回多少），
             * 这里按 16550A 的 16 字节接收 FIFO 回环，多出的字节溢出丢弃。
             */
            if (serial->mcr & MCR_LOOP) {
                for (size_t i = 0; i < size && serial->rx_count < UART_FIFO_SIZE; ++i) {
                    size_t tail = (serial->rx_head + serial->rx_count) %
                                  SERIAL_RX_CAPACITY;
                    serial->rx[tail] = data[i];
                    ++serial->rx_count;
                }
            } else {
                transmit = true;
            }
            serial->thre_pending = true;
        }
        break;
    case 1:
        /*
         * DLAB=1 时是除数锁存器高字节；否则是 IER，只有低 4 位有定义。
         * 真实 16550 在 THRI 由 0 变 1 且 THR 为空时立即报告 THR 空中断，
         * Linux 的 THRE 自检和 start_tx 都依赖这一点。
         */
        if (serial->lcr & 0x80) {
            serial->dlm = data[0];
        } else {
            uint8_t ier = data[0] & 0x0f;
            if ((ier & IER_THRI) && !(serial->ier & IER_THRI))
                serial->thre_pending = true;
            serial->ier = ier;
        }
        break;
    case 2:
        /*
         * 写方向是 FCR。bit 0 决定读 IIR 时 bit 7:6 是否报告 FIFO；
         * bit 1 清空接收 FIFO，驱动打开端口时用它丢弃旧数据。
         */
        serial->fcr = data[0] & FCR_ENABLE;
        if (data[0] & FCR_CLEAR_RCVR)
            serial_clear_rx_locked(serial);
        break;
    case 3:
        /* LCR：数据位/停止位/校验等格式配置，bit 7 是 DLAB，切换偏移 0/1 的含义。 */
        serial->lcr = data[0];
        break;
    case 4:
        /* MCR：DTR、RTS、OUT1、OUT2、LOOP 等控制位，OUT2 同时门控中断输出。 */
        serial->mcr = data[0];
        break;
    case 7:
        /* SCR 是没有任何硬件作用的暂存寄存器，驱动会写入再读回以探测 UART 是否存在。 */
        serial->scr = data[0];
        break;
    default:
        /* 偏移 5（LSR）和 6（MSR）是只读寄存器，写入被忽略。 */
        break;
    }
    serial_update_irq_locked(serial);
    pthread_mutex_unlock(&serial->lock);
    if (!transmit)
        return 0;

    /*
     * THR 写入：把字符写到 out_fd。write() 可能被信号打断（EINTR）或只写
     * 一部分，因此循环直到全部写完。返回 0 表示无法继续写入，按 I/O 错误处理。
     */
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(serial->out_fd, data + written, size - written);
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

/*
 * 处理客户机对 COM1 的 IN 指令，把读到的 8 位寄存器值写入 *value。
 *
 * 成功返回 0；端口不属于 COM1 时返回 -1 且 errno = EINVAL。
 */
int serial_handle_in(struct serial *serial, uint16_t port, uint8_t *value)
{
    if (!serial_handles_port(port)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&serial->lock);
    switch (port - COM1_PORT) {
    case 0:
        /* DLAB=1 读回除数低字节；DLAB=0 是 RBR，取出一个输入字节，队列空时读 0。 */
        if (serial->lcr & 0x80) {
            *value = serial->dll;
        } else if (serial->rx_count) {
            *value = serial->rx[serial->rx_head];
            serial->rx_head = (serial->rx_head + 1) % SERIAL_RX_CAPACITY;
            --serial->rx_count;
            pthread_cond_broadcast(&serial->rx_space);
        } else {
            *value = 0;
        }
        break;
    case 1:
        /* DLAB=1 读回除数高字节；否则读回 IER。 */
        *value = (serial->lcr & 0x80) ? serial->dlm : serial->ier;
        break;
    case 2:
        /* 读方向是 IIR。读到 THR 空中断即表示驱动已确认，挂起状态随之清除。 */
        *value = serial_iir_locked(serial);
        if ((*value & 0x0f) == IIR_THRI)
            serial->thre_pending = false;
        break;
    case 3:
        *value = serial->lcr;
        break;
    case 4:
        *value = serial->mcr;
        break;
    case 5:
        /*
         * LSR：bit 5 THRE（发送保持寄存器空）和 bit 6 TEMT（发送器空）
         * 恒为 1，表示随时可以写下一个字符，轮询发送的驱动不会卡住；
         * bit 0 DR 表示接收队列里有数据，错误位都为 0。
         */
        *value = LSR_THRE_TEMT | (serial->rx_count ? LSR_DR : 0);
        break;
    case 6:
        /*
         * MSR：高 4 位是 modem 输入线的当前状态，低 4 位是变化标志（恒为 0）。
         *
         * MCR bit 4（LOOP）置位时为回环模式：MCR 的输出线在芯片内部接到
         * MSR 的输入线，对应关系为
         *   RTS(MCR bit 1)  -> CTS(MSR bit 4)
         *   DTR(MCR bit 0)  -> DSR(MSR bit 5)
         *   OUT1(MCR bit 2) -> RI (MSR bit 6)
         *   OUT2(MCR bit 3) -> DCD(MSR bit 7)
         * Linux 8250 驱动探测端口时会做这个回环测试，结果不符就认为 UART
         * 不存在，所以这里必须按映射关系返回。
         *
         * 非回环模式下假装对端已连接：CTS、DSR、DCD 有效，RI 无效。
         */
        if (serial->mcr & 0x10) {
            *value = ((serial->mcr & 0x02) ? 0x10 : 0) |
                     ((serial->mcr & 0x01) ? 0x20 : 0) |
                     ((serial->mcr & 0x04) ? 0x40 : 0) |
                     ((serial->mcr & 0x08) ? 0x80 : 0);
        } else {
            *value = 0xb0; /* CTS, DSR and DCD asserted */
        }
        break;
    case 7:
        *value = serial->scr;
        break;
    default:
        /* serial_handles_port() 已保证偏移在 0–7，这里只是防御性兜底。 */
        *value = 0;
        break;
    }
    serial_update_irq_locked(serial);
    pthread_mutex_unlock(&serial->lock);
    return 0;
}

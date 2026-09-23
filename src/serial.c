#include "serial.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/*
 * 最小化的 16550A UART 模型，只模拟 COM1（I/O 端口 0x3f8–0x3ff），
 * 目的是让 Linux 的 8250 串口驱动和 earlyprintk 能把控制台输出写出来。
 *
 * 只实现“能输出”所需的部分：
 *   - 客户机写 THR 的字节原样写到 out_fd（通常是宿主机 stdout）；
 *   - 没有输入：RBR 恒读 0，LSR.DR 恒为 0；
 *   - 不产生中断：IIR 恒报告“无待处理中断”，驱动只能轮询；
 *   - 发送永远立即完成：LSR 恒报告发送缓冲区和移位寄存器为空；
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
 * 所有 UART 寄存器都是 8 位，因此每次访问只使用第一个字节。
 */

/* 清空所有寄存器（相当于上电复位），并记录输出目标文件描述符。 */
void serial_init(struct serial *serial, int out_fd)
{
    memset(serial, 0, sizeof(*serial));
    serial->out_fd = out_fd;
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

    /* 除了 DLAB=0 时的偏移 0（THR）会跳出 switch 去输出，其余情况都在这里返回。 */
    switch (port - COM1_PORT) {
    case 0:
        /* DLAB=1 时偏移 0 是除数锁存器低字节，只保存不输出。 */
        if (serial->lcr & 0x80) {
            serial->dll = data[0];
            return 0;
        }
        /* DLAB=0 时是 THR：跳出 switch，把字符写到宿主机。 */
        break;
    case 1:
        /*
         * DLAB=1 时是除数锁存器高字节；否则是 IER。IER 只有低 4 位有定义
         * （接收数据、发送空、线路状态、modem 状态四种中断使能），高位
         * 保留为 0。虽然本模型从不产生中断，仍保存该值供驱动读回校验。
         */
        if (serial->lcr & 0x80)
            serial->dlm = data[0];
        else
            serial->ier = data[0] & 0x0f;
        return 0;
    case 2:
        /*
         * 写方向是 FCR。只记录 bit 0（FIFO 使能），它决定读 IIR 时 bit 7:6
         * 是否报告 FIFO 已启用；清空 FIFO、触发阈值等位没有意义，直接丢弃。
         */
        serial->fcr = data[0] & 0x01;
        return 0;
    case 3:
        /* LCR：数据位/停止位/校验等格式配置，bit 7 是 DLAB，切换偏移 0/1 的含义。 */
        serial->lcr = data[0];
        return 0;
    case 4:
        /* MCR：DTR、RTS、OUT1、OUT2、LOOP 等控制位，读 MSR 时会用到。 */
        serial->mcr = data[0];
        return 0;
    case 7:
        /* SCR 是没有任何硬件作用的暂存寄存器，驱动会写入再读回以探测 UART 是否存在。 */
        serial->scr = data[0];
        return 0;
    default:
        /* 偏移 5（LSR）和 6（MSR）是只读寄存器，写入被忽略。 */
        return 0;
    }

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
    switch (port - COM1_PORT) {
    case 0:
        /* DLAB=1 读回除数低字节；DLAB=0 是 RBR，本模型没有输入，恒为 0。 */
        *value = (serial->lcr & 0x80) ? serial->dll : 0;
        break;
    case 1:
        /* DLAB=1 读回除数高字节；否则读回 IER。 */
        *value = (serial->lcr & 0x80) ? serial->dlm : serial->ier;
        break;
    case 2:
        /*
         * 读方向是 IIR。bit 0 = 1 表示没有待处理中断；启用 FIFO 后
         * bit 7:6 = 11，表示这是带可用 FIFO 的 16550A，驱动据此识别芯片型号。
         */
        *value = (serial->fcr ? 0xc0 : 0) | 0x01;
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
         * bit 0 DR（有接收数据）恒为 0，错误位也都为 0。
         */
        *value = 0x60; /* THR empty, transmitter empty */
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
    return 0;
}

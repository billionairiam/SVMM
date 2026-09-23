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

struct serial {
    int out_fd;
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t fcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    /* THR 空中断已产生但尚未被读 IIR 或写 THR 清除。 */
    bool thre_pending;
    /* 当前 INTR 输出线电平，只在变化时通知中断控制器。 */
    bool irq_level;
    bool rx_closed;
    uint8_t rx[SERIAL_RX_CAPACITY];
    size_t rx_head;
    size_t rx_count;
    serial_irq_fn set_irq;
    void *irq_opaque;
    pthread_mutex_t lock;
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

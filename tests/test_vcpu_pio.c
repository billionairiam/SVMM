#include "check.h"
#include "rtc.h"
#include "serial.h"
#include "vcpu.h"

#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * vcpu_run() 的端口 I/O 分发。可能出错的地方：
 *   1. out dx,ax 这类多字节写把所有字节都送进 THR，而不是第 j 字节落在
 *      port + j（高字节应写到 IER）；
 *   2. REP OUTSB 的每个元素被错误地分发到 port + i，而不是同一个端口；
 *   3. 多字节读只填第一个字节，其余字节残留旧数据；
 *   4. RTC 没有接到 0x70/0x71 上；
 *   5. 未模拟端口读回的不是 0xff；
 *   6. 同一未模拟端口被反复打印日志，或端口 0x80 也被当成未模拟设备；
 *   7. devices 为 NULL 时崩溃。
 *
 * 用 --wrap=ioctl 让每次 KVM_RUN 按脚本伪造一次退出，脚本最后是 HLT。
 */

struct step {
    uint8_t direction;
    uint8_t size;
    uint16_t port;
    uint32_t count;
    uint8_t data[8];
};

#define DATA_OFFSET 4096u
#define RUN_SIZE (DATA_OFFSET + 64u)

static struct kvm_run *fake_run;
static const struct step *script;
static size_t script_len;
static size_t script_pos;
static uint8_t in_results[16][8];

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request == KVM_GET_REGS)
        return -1;
    CHECK(request == KVM_RUN);

    uint8_t *data = (uint8_t *)fake_run + DATA_OFFSET;
    if (script_pos > 0 && script_pos <= script_len)
        memcpy(in_results[script_pos - 1], data, sizeof(in_results[0]));
    if (script_pos == script_len) {
        fake_run->exit_reason = KVM_EXIT_HLT;
        ++script_pos;
        return 0;
    }

    const struct step *step = &script[script_pos++];
    fake_run->exit_reason = KVM_EXIT_IO;
    fake_run->io.direction = step->direction;
    fake_run->io.size = step->size;
    fake_run->io.port = step->port;
    fake_run->io.count = step->count;
    fake_run->io.data_offset = DATA_OFFSET;
    /* IN 时预先填入垃圾值，检查 VMM 是否覆盖了每个字节。 */
    if (step->direction == KVM_EXIT_IO_IN)
        memset(data, 0xcc, 64);
    else
        memcpy(data, step->data, sizeof(step->data));
    return 0;
}

/* 运行脚本，返回 vcpu_run 的结果，并把 stderr 内容写入 log。 */
static int run_script(const struct step *steps, size_t len,
                      const struct vcpu_devices *devices,
                      char *log, size_t log_size)
{
    script = steps;
    script_len = len;
    script_pos = 0;
    memset(in_results, 0, sizeof(in_results));
    memset(fake_run, 0, RUN_SIZE);

    struct vcpu vcpu = { .fd = 1, .run = fake_run, .run_size = RUN_SIZE };
    struct vcpu_run_stats stats;
    FILE *capture = tmpfile();
    CHECK(capture);
    int saved_stderr = dup(STDERR_FILENO);
    CHECK(saved_stderr >= 0);
    CHECK(dup2(fileno(capture), STDERR_FILENO) >= 0);
    int result = vcpu_run(&vcpu, devices, &stats);
    CHECK(fflush(stderr) == 0);
    CHECK(dup2(saved_stderr, STDERR_FILENO) >= 0);
    CHECK(close(saved_stderr) == 0);

    CHECK(fseek(capture, 0, SEEK_SET) == 0);
    size_t bytes = fread(log, 1, log_size - 1, capture);
    log[bytes] = '\0';
    CHECK(fclose(capture) == 0);
    CHECK(stats.exits == len + 1);
    return result;
}

static size_t count_substr(const char *haystack, const char *needle)
{
    size_t count = 0;
    for (const char *p = haystack; (p = strstr(p, needle)); p += strlen(needle))
        ++count;
    return count;
}

int main(void)
{
    fake_run = calloc(1, RUN_SIZE);
    CHECK(fake_run);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    struct serial serial;
    serial_init(&serial, pipefd[1]);
    struct rtc rtc;
    rtc_init(&rtc);
    struct vcpu_devices devices = { .serial = &serial, .rtc = &rtc };

    const struct step steps[] = {
        /* 0: out dx,ax 到 0x3f8：'a' 进 THR，'b' 进 IER。 */
        { KVM_EXIT_IO_OUT, 2, 0x3f8, 1, { 'a', 'b' } },
        /* 1: rep outsb 到 0x3f8，3 个元素都应进 THR。 */
        { KVM_EXIT_IO_OUT, 1, 0x3f8, 3, { 'c', 'd', 'e' } },
        /* 2: in ax, 0x3f8：RBR(0)、IER(上一步写入的 'b' & 0x0f)。 */
        { KVM_EXIT_IO_IN, 2, 0x3f8, 1, { 0 } },
        /* 3、4: 选择 RTC 寄存器 D，读回 VRT。 */
        { KVM_EXIT_IO_OUT, 1, 0x70, 1, { 0x0d } },
        { KVM_EXIT_IO_IN, 1, 0x71, 1, { 0 } },
        /* 5、6: 两次读同一个未模拟端口，日志只应出现一次。 */
        { KVM_EXIT_IO_IN, 4, 0xcfc, 1, { 0 } },
        { KVM_EXIT_IO_IN, 4, 0xcfc, 1, { 0 } },
        /* 7: POST 端口静默接受。 */
        { KVM_EXIT_IO_OUT, 1, 0x80, 1, { 0x42 } },
    };
    const size_t len = sizeof(steps) / sizeof(steps[0]);
    char log[4096];
    CHECK(run_script(steps, len, &devices, log, sizeof(log)) == 0);
    CHECK(close(pipefd[1]) == 0);

    char out[16] = { 0 };
    CHECK(read(pipefd[0], out, sizeof(out)) == 4);
    CHECK(memcmp(out, "acde", 4) == 0);
    CHECK(close(pipefd[0]) == 0);

    CHECK(in_results[2][0] == 0x00);
    CHECK(in_results[2][1] == ('b' & 0x0f));
    CHECK(in_results[4][0] == 0x80);
    CHECK(memcmp(in_results[5], "\xff\xff\xff\xff", 4) == 0);
    CHECK(memcmp(in_results[6], "\xff\xff\xff\xff", 4) == 0);
    CHECK(count_substr(log, "unmodeled I/O port=0xcfc") == 1);
    CHECK(!strstr(log, "port=0x80 "));
    CHECK(!strstr(log, "port=0x3f8"));
    CHECK(!strstr(log, "port=0x7"));

    /* 7: 没有设备时串口端口也按未模拟处理。 */
    const struct step bare[] = {
        { KVM_EXIT_IO_OUT, 1, 0x3f8, 1, { 'z' } },
        { KVM_EXIT_IO_IN, 2, 0x70, 1, { 0 } },
    };
    CHECK(run_script(bare, 2, NULL, log, sizeof(log)) == 0);
    CHECK(strstr(log, "unmodeled I/O port=0x3f8"));
    CHECK(memcmp(in_results[1], "\xff\xff", 2) == 0);

    free(fake_run);
    return 0;
}

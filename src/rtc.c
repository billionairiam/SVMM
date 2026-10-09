#include "rtc.h"

#include <string.h>

/*
 * 最小化的 MC146818 CMOS RTC 模型（I/O 端口 0x70/0x71）。
 *
 * 客户机先向 0x70 写寄存器索引（bit 7 是 NMI 屏蔽位，与索引无关），
 * 再从 0x71 读写该寄存器。本模型只做 Linux 启动读时间所需的部分：
 *   - 时间寄存器按宿主机 UTC 时间实时计算，客户机写入被忽略；
 *   - 状态寄存器 A 的 UIP（更新进行中）恒为 0；
 *   - 状态寄存器 D 的 VRT 恒为 1，表示 CMOS 电池/内容有效；
 *   - 不产生中断，C 寄存器恒读 0；
 *   - 其余索引当作普通 CMOS RAM 保存。
 *
 * 没有这个设备时，未建模端口读回 0xff，UIP 位看起来一直置位，
 * Linux 会在 mc146818 读时间的路径里反复轮询直到超时，
 * 白白产生数万次 VM exit 并拖慢约 1 秒启动。
 */

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_WEEKDAY 0x06
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_REG_A 0x0a
#define RTC_REG_B 0x0b
#define RTC_REG_C 0x0c
#define RTC_REG_D 0x0d
#define RTC_CENTURY 0x32

#define RTC_A_UIP 0x80
#define RTC_B_24H 0x02
#define RTC_B_BINARY 0x04
#define RTC_D_VRT 0x80

void rtc_init(struct rtc *rtc)
{
    memset(rtc, 0, sizeof(*rtc));
    /* 32.768 kHz 时基、1024 Hz 周期中断频率，与 PC 固件的默认值一致。 */
    rtc->cmos[RTC_REG_A] = 0x26;
    /* 24 小时制、BCD 编码、关闭所有中断。 */
    rtc->cmos[RTC_REG_B] = RTC_B_24H;
}

bool rtc_handles_port(uint16_t port)
{
    return port == RTC_INDEX_PORT || port == RTC_DATA_PORT;
}

static bool is_time_register(uint8_t index)
{
    switch (index) {
    case RTC_SECONDS:
    case RTC_MINUTES:
    case RTC_HOURS:
    case RTC_WEEKDAY:
    case RTC_DAY:
    case RTC_MONTH:
    case RTC_YEAR:
    case RTC_CENTURY:
        return true;
    default:
        return false;
    }
}

/* 按状态寄存器 B 的 DM 位决定输出二进制还是 BCD。 */
static uint8_t encode(const struct rtc *rtc, unsigned value)
{
    if (rtc->cmos[RTC_REG_B] & RTC_B_BINARY)
        return (uint8_t)value;
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static uint8_t read_time(const struct rtc *rtc, uint8_t index)
{
    time_t now = rtc->now ? rtc->now() : time(NULL);
    struct tm tm;
    if (!gmtime_r(&now, &tm))
        return 0;

    switch (index) {
    case RTC_SECONDS:
        return encode(rtc, (unsigned)tm.tm_sec);
    case RTC_MINUTES:
        return encode(rtc, (unsigned)tm.tm_min);
    case RTC_HOURS:
        if (rtc->cmos[RTC_REG_B] & RTC_B_24H)
            return encode(rtc, (unsigned)tm.tm_hour);
        /* 12 小时制：1–12，bit 7 表示下午。 */
        return (uint8_t)(encode(rtc, tm.tm_hour % 12 ? (unsigned)tm.tm_hour % 12 : 12) |
                         (tm.tm_hour >= 12 ? 0x80 : 0));
    case RTC_WEEKDAY:
        return encode(rtc, (unsigned)tm.tm_wday + 1);
    case RTC_DAY:
        return encode(rtc, (unsigned)tm.tm_mday);
    case RTC_MONTH:
        return encode(rtc, (unsigned)tm.tm_mon + 1);
    case RTC_YEAR:
        return encode(rtc, (unsigned)(tm.tm_year + 1900) % 100);
    default: /* RTC_CENTURY */
        return encode(rtc, (unsigned)(tm.tm_year + 1900) / 100);
    }
}

void rtc_handle_out(struct rtc *rtc, uint16_t port, uint8_t value)
{
    if (port == RTC_INDEX_PORT) {
        rtc->index = value & 0x7f;
        return;
    }
    if (port != RTC_DATA_PORT)
        return;

    uint8_t index = rtc->index;
    /* 不允许客户机修改宿主机时钟；C/D 是只读寄存器。 */
    if (is_time_register(index) || index == RTC_REG_C || index == RTC_REG_D)
        return;
    if (index == RTC_REG_A)
        value &= (uint8_t)~RTC_A_UIP;
    rtc->cmos[index] = value;
}

uint8_t rtc_handle_in(struct rtc *rtc, uint16_t port)
{
    if (port != RTC_DATA_PORT)
        return 0xff;

    uint8_t index = rtc->index;
    if (is_time_register(index))
        return read_time(rtc, index);
    switch (index) {
    case RTC_REG_A:
        return rtc->cmos[RTC_REG_A] & (uint8_t)~RTC_A_UIP;
    case RTC_REG_C:
        return 0;
    case RTC_REG_D:
        return RTC_D_VRT;
    default:
        return rtc->cmos[index];
    }
}

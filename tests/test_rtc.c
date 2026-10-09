#include "check.h"
#include "rtc.h"

#include <stdint.h>

/*
 * 先列出 RTC 模型可能出错、且会让 Linux 读错时间或卡住的地方：
 *   1. 状态寄存器 A 的 UIP 一直为 1：mc146818_get_time() 会反复轮询；
 *   2. 状态寄存器 D 的 VRT 为 0：内核认为 CMOS 电池失效；
 *   3. BCD 编码错误（例如把 59 编成 0x3b）；
 *   4. 客户机切换到二进制模式（寄存器 B bit 2）后仍返回 BCD；
 *   5. 12 小时制时小时值和 PM 位（bit 7）错误，尤其是 0 点和 12 点；
 *   6. 索引端口写入时没有去掉 bit 7（NMI 屏蔽位），读到错误寄存器；
 *   7. 客户机写时间寄存器或只读的 C/D 寄存器后改变了返回值；
 *   8. 普通 CMOS RAM 写入后读不回来；
 *   9. 世纪寄存器 0x32 错误，导致年份差 100 年；
 *  10. 不属于 RTC 的端口被认领。
 */

/* 2024-02-29 23:59:58 UTC，星期四。 */
static time_t fixed_evening(void)
{
    return 1709251198;
}

/* 2024-03-01 00:05:09 UTC，星期五。 */
static time_t fixed_midnight(void)
{
    return 1709251509;
}

/* 2024-03-01 12:00:00 UTC。 */
static time_t fixed_noon(void)
{
    return 1709294400;
}

static uint8_t rtc_read(struct rtc *rtc, uint8_t index)
{
    rtc_handle_out(rtc, RTC_INDEX_PORT, index);
    return rtc_handle_in(rtc, RTC_DATA_PORT);
}

static void rtc_write(struct rtc *rtc, uint8_t index, uint8_t value)
{
    rtc_handle_out(rtc, RTC_INDEX_PORT, index);
    rtc_handle_out(rtc, RTC_DATA_PORT, value);
}

int main(void)
{
    struct rtc rtc;
    rtc_init(&rtc);
    rtc.now = fixed_evening;

    /* 10 */
    CHECK(rtc_handles_port(0x70));
    CHECK(rtc_handles_port(0x71));
    CHECK(!rtc_handles_port(0x72));
    CHECK(!rtc_handles_port(0x80));

    /* 1、2 */
    CHECK((rtc_read(&rtc, 0x0a) & 0x80) == 0);
    rtc_write(&rtc, 0x0a, 0xa6);
    CHECK(rtc_read(&rtc, 0x0a) == 0x26);
    CHECK(rtc_read(&rtc, 0x0d) & 0x80);

    /* 3：默认 24 小时制 BCD。 */
    CHECK(rtc_read(&rtc, 0x00) == 0x58);
    CHECK(rtc_read(&rtc, 0x02) == 0x59);
    CHECK(rtc_read(&rtc, 0x04) == 0x23);
    CHECK(rtc_read(&rtc, 0x06) == 0x05);
    CHECK(rtc_read(&rtc, 0x07) == 0x29);
    CHECK(rtc_read(&rtc, 0x08) == 0x02);
    CHECK(rtc_read(&rtc, 0x09) == 0x24);
    /* 9 */
    CHECK(rtc_read(&rtc, 0x32) == 0x20);

    /* 6：带 NMI 屏蔽位的索引仍然选中秒寄存器。 */
    rtc_handle_out(&rtc, RTC_INDEX_PORT, 0x80);
    CHECK(rtc_handle_in(&rtc, RTC_DATA_PORT) == 0x58);

    /* 7 */
    rtc_write(&rtc, 0x00, 0x11);
    rtc_write(&rtc, 0x09, 0x99);
    rtc_write(&rtc, 0x0c, 0xff);
    rtc_write(&rtc, 0x0d, 0x00);
    CHECK(rtc_read(&rtc, 0x00) == 0x58);
    CHECK(rtc_read(&rtc, 0x09) == 0x24);
    CHECK(rtc_read(&rtc, 0x0c) == 0x00);
    CHECK(rtc_read(&rtc, 0x0d) & 0x80);

    /* 4：二进制 + 24 小时制。 */
    rtc_write(&rtc, 0x0b, 0x06);
    CHECK(rtc_read(&rtc, 0x00) == 58);
    CHECK(rtc_read(&rtc, 0x04) == 23);
    CHECK(rtc_read(&rtc, 0x09) == 24);
    CHECK(rtc_read(&rtc, 0x32) == 20);

    /* 5：12 小时制 BCD。23 点 -> PM 11，0 点 -> AM 12，12 点 -> PM 12。 */
    rtc_write(&rtc, 0x0b, 0x00);
    CHECK(rtc_read(&rtc, 0x04) == (0x80 | 0x11));
    rtc.now = fixed_midnight;
    CHECK(rtc_read(&rtc, 0x04) == 0x12);
    CHECK(rtc_read(&rtc, 0x02) == 0x05);
    CHECK(rtc_read(&rtc, 0x00) == 0x09);
    CHECK(rtc_read(&rtc, 0x06) == 0x06);
    CHECK(rtc_read(&rtc, 0x07) == 0x01);
    CHECK(rtc_read(&rtc, 0x08) == 0x03);
    rtc.now = fixed_noon;
    CHECK(rtc_read(&rtc, 0x04) == (0x80 | 0x12));

    /* 8 */
    rtc_write(&rtc, 0x0e, 0x5a);
    rtc_write(&rtc, 0x7f, 0xa5);
    CHECK(rtc_read(&rtc, 0x0e) == 0x5a);
    CHECK(rtc_read(&rtc, 0x7f) == 0xa5);
    return 0;
}

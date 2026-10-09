#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define RTC_INDEX_PORT 0x70u
#define RTC_DATA_PORT 0x71u
#define RTC_CMOS_SIZE 128u

struct rtc {
    uint8_t index;
    uint8_t cmos[RTC_CMOS_SIZE];
    /* 时间来源；为 NULL 时使用 time(NULL)。测试可替换成固定时间。 */
    time_t (*now)(void);
};

void rtc_init(struct rtc *rtc);
bool rtc_handles_port(uint16_t port);
void rtc_handle_out(struct rtc *rtc, uint16_t port, uint8_t value);
uint8_t rtc_handle_in(struct rtc *rtc, uint16_t port);

#endif

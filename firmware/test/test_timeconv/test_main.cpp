// Host unit tests for calendar maths and the DS3231 register image:  pio test -e native
#include <unity.h>

#include "timeconv.h"

using namespace timeconv;

void setUp() {}
void tearDown() {}

static void test_known_dates() {
    Civil c = fromEpoch(1704067200UL);  // 2024-01-01 00:00:00 UTC, a Monday
    TEST_ASSERT_EQUAL(2024, c.year);
    TEST_ASSERT_EQUAL(1, c.month);
    TEST_ASSERT_EQUAL(1, c.day);
    TEST_ASSERT_EQUAL(0, c.hour);
    TEST_ASSERT_EQUAL(1, c.weekday);

    c = fromEpoch(1709251199UL);  // 2024-02-29 23:59:59 UTC (leap day), a Thursday
    TEST_ASSERT_EQUAL(2, c.month);
    TEST_ASSERT_EQUAL(29, c.day);
    TEST_ASSERT_EQUAL(23, c.hour);
    TEST_ASSERT_EQUAL(59, c.minute);
    TEST_ASSERT_EQUAL(59, c.second);
    TEST_ASSERT_EQUAL(4, c.weekday);

    c = fromEpoch(1794787200UL);  // 2026-11-16 00:00:00 UTC, a Monday
    TEST_ASSERT_EQUAL(2026, c.year);
    TEST_ASSERT_EQUAL(11, c.month);
    TEST_ASSERT_EQUAL(16, c.day);
    TEST_ASSERT_EQUAL(1, c.weekday);
}

static void test_epoch_roundtrip() {
    // A prime step walks through every time of day, month end and leap year up to 2099.
    for (uint32_t e = MIN_VALID_EPOCH; e < 4102444800UL - 400000UL; e += 345679UL)
        TEST_ASSERT_EQUAL_UINT32(e, toEpoch(fromEpoch(e)));
}

static void test_bcd() {
    TEST_ASSERT_EQUAL_HEX8(0x59, toBcd(59));
    TEST_ASSERT_EQUAL(59, fromBcd(0x59));
    for (uint8_t v = 0; v < 100; v++) TEST_ASSERT_EQUAL(v, fromBcd(toBcd(v)));
}

static void test_rtc_regs_known() {
    uint8_t r[RTC_REGS];
    toRtcRegs(1709251199UL, r);  // 2024-02-29 23:59:59, Thursday
    const uint8_t expect[RTC_REGS] = {0x59, 0x59, 0x23, 5, 0x29, 0x02, 0x24};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, r, RTC_REGS);
}

static void test_rtc_regs_roundtrip() {
    for (uint32_t e = MIN_VALID_EPOCH; e < 4102444800UL - 400000UL; e += 345679UL) {
        uint8_t r[RTC_REGS];
        uint32_t back = 0;
        toRtcRegs(e, r);
        TEST_ASSERT_TRUE(fromRtcRegs(r, back));
        TEST_ASSERT_EQUAL_UINT32(e, back);
    }
}

static void test_rtc_ignores_century_and_12h() {
    uint8_t r[RTC_REGS] = {0x00, 0x30, 0x40 | 0x20 | 0x07, 2, 0x16, 0x80 | 0x11, 0x26};  // 7:30 PM, century bit set
    uint32_t e = 0;
    TEST_ASSERT_TRUE(fromRtcRegs(r, e));
    Civil c = fromEpoch(e);
    TEST_ASSERT_EQUAL(19, c.hour);
    TEST_ASSERT_EQUAL(30, c.minute);
    TEST_ASSERT_EQUAL(11, c.month);
    r[2] = 0x40 | 0x12;  // 12 AM = midnight
    TEST_ASSERT_TRUE(fromRtcRegs(r, e));
    TEST_ASSERT_EQUAL(0, fromEpoch(e).hour);
}

static void test_rtc_rejects_garbage() {
    uint32_t e = 0;
    const uint8_t blank[RTC_REGS] = {0, 0, 0, 1, 1, 1, 0};  // factory default 2000-01-01: not a real time
    TEST_ASSERT_FALSE(fromRtcRegs(blank, e));
    const uint8_t ff[RTC_REGS] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // bus floating
    TEST_ASSERT_FALSE(fromRtcRegs(ff, e));
    const uint8_t month13[RTC_REGS] = {0x00, 0x00, 0x12, 1, 0x15, 0x13, 0x26};
    TEST_ASSERT_FALSE(fromRtcRegs(month13, e));
    const uint8_t hour24[RTC_REGS] = {0x00, 0x00, 0x24, 1, 0x15, 0x06, 0x26};
    TEST_ASSERT_FALSE(fromRtcRegs(hour24, e));
    const uint8_t day0[RTC_REGS] = {0x00, 0x00, 0x12, 1, 0x00, 0x06, 0x26};
    TEST_ASSERT_FALSE(fromRtcRegs(day0, e));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_known_dates);
    RUN_TEST(test_epoch_roundtrip);
    RUN_TEST(test_bcd);
    RUN_TEST(test_rtc_regs_known);
    RUN_TEST(test_rtc_regs_roundtrip);
    RUN_TEST(test_rtc_ignores_century_and_12h);
    RUN_TEST(test_rtc_rejects_garbage);
    return UNITY_END();
}

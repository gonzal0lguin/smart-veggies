// Host unit tests: watering programs.  pio test -e native
#include <unity.h>

#include "scheduler.h"

using namespace sched;

void setUp() {}
void tearDown() {}

// 2026-11-16 00:00:00 UTC is a Monday.
constexpr uint32_t MON = 1794787200u;

static uint32_t at(uint32_t day0, int day, int h, int m, int s = 0) { return day0 + day * 86400 + h * 3600 + m * 60 + s; }

static void test_weekday() {
    TEST_ASSERT_EQUAL(4, weekday(0));  // 1970-01-01 was a Thursday
    TEST_ASSERT_EQUAL(1, weekday(MON));
    TEST_ASSERT_EQUAL(0, weekday(MON + 6 * 86400));
    TEST_ASSERT_EQUAL(7 * 60 + 30, minuteOfDay(at(MON, 0, 7, 30, 59)));
}

static void test_fires_once_at_start_minute() {
    Scheduler s;
    s.programs[2].enabled = 1;
    s.programs[2].startMin = 7 * 60;
    uint8_t out[4];
    TEST_ASSERT_EQUAL(0, s.due(at(MON, 0, 6, 59, 30), out, 4));
    TEST_ASSERT_EQUAL(1, s.due(at(MON, 0, 7, 0, 1), out, 4));
    TEST_ASSERT_EQUAL(2, out[0]);
    for (int sec = 2; sec < 60; sec++) TEST_ASSERT_EQUAL(0, s.due(at(MON, 0, 7, 0, sec), out, 4));
    TEST_ASSERT_EQUAL(0, s.due(at(MON, 0, 7, 1), out, 4));
    TEST_ASSERT_EQUAL(1, s.due(at(MON, 1, 7, 0), out, 4));  // next day (after a long gap)
}

static void test_weekday_mask() {
    Scheduler s;
    s.programs[0].enabled = 1;
    s.programs[0].startMin = 600;
    s.programs[0].days = (1 << 1) | (1 << 3);  // Monday, Wednesday
    uint8_t out[4];
    int fired = 0;
    for (int d = 0; d < 7; d++) {
        s.due(at(MON, d, 9, 59), out, 4);
        int n = s.due(at(MON, d, 10, 0), out, 4);
        TEST_ASSERT_EQUAL((d == 0 || d == 2) ? 1 : 0, n);
        fired += n;
    }
    TEST_ASSERT_EQUAL(2, fired);
}

static void test_short_gap_is_caught_up_long_gap_is_not() {
    Scheduler s;
    s.programs[0].enabled = 1;
    s.programs[0].startMin = 8 * 60;
    uint8_t out[4];
    s.due(at(MON, 0, 7, 59), out, 4);
    TEST_ASSERT_EQUAL(1, s.due(at(MON, 0, 8, 1, 30), out, 4));  // loop was busy for 90 s
    Scheduler t = Scheduler();
    t.programs[0] = s.programs[0];
    t.due(at(MON, 0, 7, 0), out, 4);
    TEST_ASSERT_EQUAL(0, t.due(at(MON, 0, 8, 30), out, 4));  // clock jumped: do not replay
    TEST_ASSERT_EQUAL(0, t.due(at(MON, 0, 7, 0), out, 4));   // clock went backwards
}

static void test_first_call_in_start_minute_fires() {
    Scheduler s;
    s.programs[0].enabled = 1;
    s.programs[0].startMin = 8 * 60;
    uint8_t out[4];
    TEST_ASSERT_EQUAL(1, s.due(at(MON, 0, 8, 0, 20), out, 4));  // booted during the start minute
}

static void test_disabled_and_multiple() {
    Scheduler s;
    for (int i = 0; i < 3; i++) s.programs[i].startMin = 300;
    s.programs[0].enabled = s.programs[2].enabled = 1;
    uint8_t out[4];
    TEST_ASSERT_EQUAL(2, s.due(at(MON, 0, 5, 0), out, 4));
    TEST_ASSERT_EQUAL(0, out[0]);
    TEST_ASSERT_EQUAL(2, out[1]);
}

static void test_new_day() {
    Scheduler s;
    TEST_ASSERT_FALSE(s.newDay(at(MON, 0, 12, 0)));
    TEST_ASSERT_FALSE(s.newDay(at(MON, 0, 23, 59, 59)));
    TEST_ASSERT_TRUE(s.newDay(at(MON, 1, 0, 0)));
    TEST_ASSERT_FALSE(s.newDay(at(MON, 1, 0, 1)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_weekday);
    RUN_TEST(test_fires_once_at_start_minute);
    RUN_TEST(test_weekday_mask);
    RUN_TEST(test_short_gap_is_caught_up_long_gap_is_not);
    RUN_TEST(test_first_call_in_start_minute_fires);
    RUN_TEST(test_disabled_and_multiple);
    RUN_TEST(test_new_day);
    return UNITY_END();
}

#pragma once
// Pure calendar maths: Unix epoch <-> civil date/time <-> DS3231 register image.
// No hardware dependencies, so it is unit-tested on the PC (test/test_timeconv).
#include <stddef.h>
#include <stdint.h>

namespace timeconv {

struct Civil {
    uint16_t year;   // 2000..2099 for the RTC
    uint8_t month;   // 1..12
    uint8_t day;     // 1..31
    uint8_t hour, minute, second;
    uint8_t weekday;  // 0 = Sunday .. 6 = Saturday
};

Civil fromEpoch(uint32_t epoch);
uint32_t toEpoch(const Civil& c);  // weekday is ignored

uint8_t toBcd(uint8_t v);
uint8_t fromBcd(uint8_t v);

// DS3231 time registers 0x00..0x06 (24 h mode). The RTC is kept in UTC.
constexpr size_t RTC_REGS = 7;
void toRtcRegs(uint32_t epoch, uint8_t regs[RTC_REGS]);
// False if the registers do not hold a plausible date (blank or corrupted chip).
bool fromRtcRegs(const uint8_t regs[RTC_REGS], uint32_t& epoch);

// Earliest time accepted as "real": 2024-01-01. Anything before means "not set".
constexpr uint32_t MIN_VALID_EPOCH = 1704067200UL;

}  // namespace timeconv

#include "timeconv.h"

namespace timeconv {

// Days since 1970-01-01 <-> civil date (Howard Hinnant's algorithms).
static int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

Civil fromEpoch(uint32_t epoch) {
    Civil c;
    uint32_t days = epoch / 86400UL, rem = epoch % 86400UL;
    c.hour = (uint8_t)(rem / 3600);
    c.minute = (uint8_t)(rem % 3600 / 60);
    c.second = (uint8_t)(rem % 60);
    c.weekday = (uint8_t)((days + 4) % 7);  // 1970-01-01 was a Thursday

    const int32_t z = (int32_t)days + 719468;
    const int32_t era = z / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    c.day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    c.month = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    c.year = (uint16_t)((int32_t)yoe + era * 400 + (c.month <= 2));
    return c;
}

uint32_t toEpoch(const Civil& c) {
    return (uint32_t)daysFromCivil(c.year, c.month, c.day) * 86400UL + c.hour * 3600UL + c.minute * 60UL + c.second;
}

uint8_t toBcd(uint8_t v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
uint8_t fromBcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

void toRtcRegs(uint32_t epoch, uint8_t regs[RTC_REGS]) {
    Civil c = fromEpoch(epoch);
    regs[0] = toBcd(c.second);
    regs[1] = toBcd(c.minute);
    regs[2] = toBcd(c.hour);  // bit 6 clear = 24 h mode
    regs[3] = (uint8_t)(c.weekday + 1);
    regs[4] = toBcd(c.day);
    regs[5] = toBcd(c.month);
    regs[6] = toBcd((uint8_t)(c.year % 100));
}

static bool bcdOk(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

bool fromRtcRegs(const uint8_t regs[RTC_REGS], uint32_t& epoch) {
    uint8_t hourReg = regs[2];
    uint8_t hour;
    if (hourReg & 0x40) {  // 12 h mode (never written by us, but decode it anyway)
        if (!bcdOk(hourReg & 0x1F)) return false;
        hour = (uint8_t)(fromBcd(hourReg & 0x1F) % 12 + ((hourReg & 0x20) ? 12 : 0));
    } else {
        if (!bcdOk(hourReg & 0x3F)) return false;
        hour = fromBcd(hourReg & 0x3F);
    }
    uint8_t sec = regs[0] & 0x7F, min = regs[1] & 0x7F, day = regs[4] & 0x3F, mon = regs[5] & 0x1F;
    if (!bcdOk(sec) || !bcdOk(min) || !bcdOk(day) || !bcdOk(mon) || !bcdOk(regs[6])) return false;
    Civil c;
    c.second = fromBcd(sec);
    c.minute = fromBcd(min);
    c.hour = hour;
    c.day = fromBcd(day);
    c.month = fromBcd(mon);
    c.year = (uint16_t)(2000 + fromBcd(regs[6]));
    c.weekday = 0;
    if (c.second > 59 || c.minute > 59 || c.hour > 23 || c.day < 1 || c.day > 31 || c.month < 1 || c.month > 12) return false;
    epoch = toEpoch(c);
    return epoch >= MIN_VALID_EPOCH;
}

}  // namespace timeconv

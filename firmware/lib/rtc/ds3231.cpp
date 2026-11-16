#include "ds3231.h"

#include "timeconv.h"

static const uint8_t REG_TIME = 0x00, REG_STATUS = 0x0F, REG_TEMP = 0x11;
static const uint8_t STATUS_OSF = 0x80;

bool Ds3231::readRegs(uint8_t reg, uint8_t* buf, uint8_t n) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    if (wire_.endTransmission(false) != 0) return false;
    if (wire_.requestFrom(addr_, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)wire_.read();
    return true;
}

bool Ds3231::writeRegs(uint8_t reg, const uint8_t* buf, uint8_t n) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    wire_.write(buf, n);
    return wire_.endTransmission() == 0;
}

bool Ds3231::probe() {
    wire_.beginTransmission(addr_);
    return wire_.endTransmission() == 0;
}

bool Ds3231::read(uint32_t& epoch) {
    uint8_t status, regs[timeconv::RTC_REGS];
    if (!readRegs(REG_STATUS, &status, 1) || (status & STATUS_OSF)) return false;
    if (!readRegs(REG_TIME, regs, sizeof(regs))) return false;
    return timeconv::fromRtcRegs(regs, epoch);
}

bool Ds3231::write(uint32_t epoch) {
    uint8_t regs[timeconv::RTC_REGS], status;
    timeconv::toRtcRegs(epoch, regs);
    if (!writeRegs(REG_TIME, regs, sizeof(regs))) return false;
    if (!readRegs(REG_STATUS, &status, 1)) return false;
    status &= (uint8_t)~STATUS_OSF;
    return writeRegs(REG_STATUS, &status, 1);
}

bool Ds3231::temperature(float& celsius) {
    uint8_t b[2];
    if (!readRegs(REG_TEMP, b, 2)) return false;
    celsius = (int16_t)((uint16_t)b[0] << 8 | b[1]) / 256.0f;
    return true;
}

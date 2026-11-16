#include "ads1115.h"

namespace {
constexpr uint8_t REG_CONVERSION = 0x00;
constexpr uint8_t REG_CONFIG = 0x01;
// OS=1 (start), PGA=001 (+/-4.096 V), MODE=1 (single shot), DR=100 (128 SPS), comparator off
constexpr uint16_t CONFIG_BASE = 0x8000 | 0x0200 | 0x0100 | 0x0080 | 0x0003;
}  // namespace

bool Ads1115::writeReg(uint8_t reg, uint16_t value) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    wire_.write((uint8_t)(value >> 8));
    wire_.write((uint8_t)(value & 0xFF));
    return wire_.endTransmission() == 0;
}

bool Ads1115::readReg(uint8_t reg, uint16_t& value) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    if (wire_.endTransmission() != 0) return false;
    if (wire_.requestFrom((int)addr_, 2) != 2) return false;
    value = (uint16_t)wire_.read() << 8;
    value |= (uint16_t)wire_.read();
    return true;
}

bool Ads1115::probe() {
    uint16_t cfg;
    return readReg(REG_CONFIG, cfg);
}

bool Ads1115::readMilliVolts(uint8_t channel, float& mv) {
    if (channel > 3) return false;
    uint16_t mux = (uint16_t)(0x4 + channel) << 12;  // AINx vs GND
    if (!writeReg(REG_CONFIG, CONFIG_BASE | mux)) return false;
    uint16_t cfg = 0;
    uint32_t start = millis();
    do {  // conversion takes ~8 ms; OS bit goes high when done
        delay(2);
        if (!readReg(REG_CONFIG, cfg)) return false;
        if (millis() - start > 50) return false;
    } while (!(cfg & 0x8000));
    uint16_t raw;
    if (!readReg(REG_CONVERSION, raw)) return false;
    mv = (int16_t)raw * 0.125f;
    return true;
}

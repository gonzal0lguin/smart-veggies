#include "soil_moisture.h"

#include <math.h>

uint8_t SoilMoisture::begin(const uint8_t* adsAddresses, uint8_t adsCount, const uint8_t* adcPins,
                            uint8_t adcPinCount) {
    count_ = 0;
    usingAds_ = false;
    uint8_t found = 0;
    for (uint8_t i = 0; i < adsCount && found < 4; i++) {
        Ads1115* adc = new Ads1115(adsAddresses[i]);
        if (!adc->probe()) {
            delete adc;
            continue;
        }
        ads_[found] = adc;
        for (uint8_t in = 0; in < 4 && count_ < MAX_CHANNELS; in++) {
            ch_[count_].ads = (int8_t)found;
            ch_[count_].input = in;
            count_++;
        }
        found++;
    }
    if (found > 0) {
        usingAds_ = true;
        return count_;
    }
    for (uint8_t i = 0; i < adcPinCount && count_ < MAX_CHANNELS; i++) {
        pinMode(adcPins[i], INPUT);
        ch_[count_].ads = -1;
        ch_[count_].input = adcPins[i];
        count_++;
    }
    return count_;
}

void SoilMoisture::describe(uint8_t ch, char* out, size_t cap) const {
    if (ch >= count_) {
        snprintf(out, cap, "-");
    } else if (ch_[ch].ads >= 0) {
        snprintf(out, cap, "0x%02X/A%u", ads_[ch_[ch].ads]->address(), ch_[ch].input);
    } else {
        snprintf(out, cap, "GPIO%u", ch_[ch].input);
    }
}

bool SoilMoisture::readMilliVolts(uint8_t ch, float& mv) {
    if (ch >= count_) return false;
    if (ch_[ch].ads >= 0) return ads_[ch_[ch].ads]->readMilliVolts(ch_[ch].input, mv);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(ch_[ch].input);
    mv = sum / 16.0f;
    return true;
}

float SoilMoisture::toPercent(uint8_t ch, float mv) const {
    if (ch >= count_ || !cal_[ch].valid()) return NAN;
    float pct = 100.0f * (cal_[ch].dryMv - mv) / (cal_[ch].dryMv - cal_[ch].wetMv);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

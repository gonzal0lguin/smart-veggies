#pragma once
// Capacitive soil moisture channels. Each channel is one sensor read either
// through an ADS1115 input or (fallback, for bench screening) an ESP32 ADC1 pin.
// The sensor output goes DOWN as the soil gets wetter.
#include <Arduino.h>

#include "ads1115.h"

class SoilMoisture {
   public:
    static constexpr uint8_t MAX_CHANNELS = 16;

    struct Calibration {
        float dryMv = 0;  // reading in air
        float wetMv = 0;  // reading in water
        bool valid() const { return dryMv > wetMv + 100.0f; }
    };

    // Probes the given ADS1115 addresses (4 channels each). If none answers,
    // uses the fallback ADC pins instead. Returns the number of channels.
    uint8_t begin(const uint8_t* adsAddresses, uint8_t adsCount, const uint8_t* adcPins, uint8_t adcPinCount);

    uint8_t channels() const { return count_; }
    bool usingAds() const { return usingAds_; }
    // Human-readable input name, e.g. "0x48/A2" or "GPIO36".
    void describe(uint8_t ch, char* out, size_t cap) const;

    bool readMilliVolts(uint8_t ch, float& mv);
    // 0 % = dry calibration point, 100 % = wet point. NAN when not calibrated.
    float toPercent(uint8_t ch, float mv) const;
    Calibration& calibration(uint8_t ch) { return cal_[ch]; }

   private:
    struct Channel {
        int8_t ads = -1;    // index into ads_ (or -1 for an ADC pin)
        uint8_t input = 0;  // ADS input 0..3 or GPIO number
    };
    Ads1115* ads_[4] = {};
    Channel ch_[MAX_CHANNELS];
    Calibration cal_[MAX_CHANNELS];
    uint8_t count_ = 0;
    bool usingAds_ = false;
};

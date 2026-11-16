#pragma once
// Minimal ADS1115 driver: single-shot, single-ended reads, +/-4.096 V range
// (0.125 mV per bit), 128 samples/s. Enough for slow soil moisture sensors.
#include <Arduino.h>
#include <Wire.h>

class Ads1115 {
   public:
    explicit Ads1115(uint8_t address, TwoWire& wire = Wire) : addr_(address), wire_(wire) {}

    bool probe();
    // Reads channel 0..3 against GND. Returns false on I2C error/timeout.
    bool readMilliVolts(uint8_t channel, float& mv);
    uint8_t address() const { return addr_; }

   private:
    bool writeReg(uint8_t reg, uint16_t value);
    bool readReg(uint8_t reg, uint16_t& value);

    uint8_t addr_;
    TwoWire& wire_;
};

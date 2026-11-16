#pragma once
// DS3231 battery-backed clock on I2C (0x68). Kept in UTC. The node reads it at
// boot so schedules run after a power cut even when the gateway is unreachable.
#include <Arduino.h>
#include <Wire.h>

class Ds3231 {
   public:
    explicit Ds3231(TwoWire& wire = Wire, uint8_t address = 0x68) : wire_(wire), addr_(address) {}

    bool probe();
    // False if the chip is missing, its oscillator stopped (flat/absent coin
    // cell, time lost) or the date is implausible.
    bool read(uint32_t& epoch);
    // Writes the time and clears the oscillator-stop flag.
    bool write(uint32_t epoch);
    // Die temperature, 0.25 C resolution: a free reading of how hot the box is.
    bool temperature(float& celsius);

   private:
    bool readRegs(uint8_t reg, uint8_t* buf, uint8_t n);
    bool writeRegs(uint8_t reg, const uint8_t* buf, uint8_t n);

    TwoWire& wire_;
    uint8_t addr_;
};

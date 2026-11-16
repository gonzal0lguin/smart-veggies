#pragma once
// Relay-driven valve outputs. A valve can only be opened WITH a time limit:
// tick() closes it when the limit expires, so no code path can leave water
// running forever. Call tick() often from the loop.
#include <Arduino.h>

class ValveBank {
   public:
    static constexpr uint8_t MAX_VALVES = 8;

    ValveBank(const int8_t* pins, uint8_t count, bool activeLow);

    // Drives every output to the OFF level before making it an output.
    void begin();
    // Opens valve `i` for at most `maxMs`. With exclusive mode (default) all
    // other valves are closed first. Returns false for a bad index or maxMs == 0.
    bool open(uint8_t i, uint32_t maxMs);
    void close(uint8_t i);
    void closeAll();
    void tick();

    bool isOpen(uint8_t i) const { return i < count_ && open_[i]; }
    bool anyOpen() const;
    uint32_t remainingMs(uint8_t i) const;
    uint8_t count() const { return count_; }
    void setExclusive(bool on) { exclusive_ = on; }

   private:
    void drive(uint8_t i, bool on);

    int8_t pins_[MAX_VALVES] = {};
    uint8_t count_;
    bool activeLow_;
    bool exclusive_ = true;
    bool open_[MAX_VALVES] = {};
    uint32_t openedAt_[MAX_VALVES] = {};
    uint32_t limitMs_[MAX_VALVES] = {};
};

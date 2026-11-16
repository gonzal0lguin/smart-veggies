#pragma once
// Watering programs: "zone Z, on these weekdays, at HH:MM, this much".
// Pure logic, host-tested. Works in LOCAL time (epoch seconds + tz offset).
#include <stddef.h>
#include <stdint.h>

namespace sched {

constexpr uint8_t MAX_PROGRAMS = 12;
constexpr uint8_t EVERY_DAY = 0x7F;

struct Program {
    uint8_t enabled = 0;
    uint8_t zone = 0;
    uint8_t days = EVERY_DAY;    // bit 0 = Sunday ... bit 6 = Saturday
    uint16_t startMin = 0;       // minutes after local midnight
    uint32_t ml = 0;             // volume target, 0 = timed run
    uint16_t seconds = 0;        // duration (timed) or time limit (volume, 0 = zone default)
    uint8_t skipAbovePct = 0;    // skip if the zone's soil moisture is at/above this (0 = always run)
};

inline uint8_t weekday(uint32_t localEpoch) { return (uint8_t)((localEpoch / 86400 + 4) % 7); }  // 0 = Sunday
inline uint16_t minuteOfDay(uint32_t localEpoch) { return (uint16_t)(localEpoch % 86400 / 60); }

class Scheduler {
   public:
    Program programs[MAX_PROGRAMS];

    // Call at least once a minute with the local time. Writes the indices of
    // programs that start now into `out`, returns how many. Each program fires
    // at most once per start minute. After a gap (boot, clock jump) of more
    // than `catchUpMin` minutes, missed starts are NOT replayed.
    uint8_t due(uint32_t localEpoch, uint8_t* out, uint8_t cap, uint8_t catchUpMin = 2);

    // True once each time the local date changes (for daily counters).
    bool newDay(uint32_t localEpoch);

   private:
    uint32_t lastMinute_ = 0;  // absolute minute already processed, 0 = none yet
    uint32_t lastDay_ = 0;
};

}  // namespace sched

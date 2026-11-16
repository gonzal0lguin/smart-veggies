#include "scheduler.h"

namespace sched {

uint8_t Scheduler::due(uint32_t localEpoch, uint8_t* out, uint8_t cap, uint8_t catchUpMin) {
    uint32_t minute = localEpoch / 60;
    if (lastMinute_ == 0 || minute < lastMinute_ || minute - lastMinute_ > catchUpMin) lastMinute_ = minute - 1;
    uint8_t n = 0;
    for (uint32_t m = lastMinute_ + 1; m <= minute; m++) {
        uint32_t t = m * 60;
        for (uint8_t i = 0; i < MAX_PROGRAMS && n < cap; i++) {
            const Program& p = programs[i];
            if (p.enabled && (p.days & (1u << weekday(t))) && p.startMin == minuteOfDay(t)) out[n++] = i;
        }
    }
    lastMinute_ = minute;
    return n;
}

bool Scheduler::newDay(uint32_t localEpoch) {
    uint32_t day = localEpoch / 86400;
    if (lastDay_ == 0) lastDay_ = day;
    if (day == lastDay_) return false;
    lastDay_ = day;
    return true;
}

}  // namespace sched

#include "valves.h"

ValveBank::ValveBank(const int8_t* pins, uint8_t count, bool activeLow)
    : count_(count > MAX_VALVES ? MAX_VALVES : count), activeLow_(activeLow) {
    for (uint8_t i = 0; i < count_; i++) pins_[i] = pins[i];
}

void ValveBank::drive(uint8_t i, bool on) { digitalWrite(pins_[i], (on != activeLow_) ? HIGH : LOW); }

void ValveBank::begin() {
    for (uint8_t i = 0; i < count_; i++) {
        drive(i, false);  // set the level first so the relay does not click at start-up
        pinMode(pins_[i], OUTPUT);
        drive(i, false);
        open_[i] = false;
    }
}

bool ValveBank::open(uint8_t i, uint32_t maxMs) {
    if (i >= count_ || maxMs == 0) return false;
    if (exclusive_)
        for (uint8_t j = 0; j < count_; j++)
            if (j != i) close(j);
    open_[i] = true;
    openedAt_[i] = millis();
    limitMs_[i] = maxMs;
    drive(i, true);
    return true;
}

void ValveBank::close(uint8_t i) {
    if (i >= count_) return;
    drive(i, false);
    open_[i] = false;
}

void ValveBank::closeAll() {
    for (uint8_t i = 0; i < count_; i++) close(i);
}

void ValveBank::tick() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < count_; i++)
        if (open_[i] && now - openedAt_[i] >= limitMs_[i]) close(i);
}

bool ValveBank::anyOpen() const {
    for (uint8_t i = 0; i < count_; i++)
        if (open_[i]) return true;
    return false;
}

uint32_t ValveBank::remainingMs(uint8_t i) const {
    if (!isOpen(i)) return 0;
    uint32_t elapsed = millis() - openedAt_[i];
    return elapsed >= limitMs_[i] ? 0 : limitMs_[i] - elapsed;
}

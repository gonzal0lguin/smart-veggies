#include "irrigation.h"

namespace irr {

Reject Controller::check(const Request& r) const {
    if (r.zone >= cfg_.zones || r.zone >= MAX_ZONES) return Reject::BadZone;
    const ZoneConfig& z = cfg_.zone[r.zone];
    if (!z.enabled) return Reject::Disabled;
    if (fault_[r.zone]) return Reject::Locked;
    if (lockoutMask_ & (1u << r.zone)) return Reject::LockedOut;
    if (r.targetMl == 0 && r.maxS == 0) return Reject::Invalid;
    if (r.targetMl && z.flowSensor == NONE && z.nominalMlPerMin == 0) return Reject::Invalid;
    if (z.flowSensor != NONE && z.flowSensor >= MAX_FLOW) return Reject::Invalid;
    if (dailyMl_[r.zone] >= z.maxDailyMl) return Reject::DailyLimit;
    if (z.flowSensor != NONE && (leakMask_ & (1u << z.flowSensor))) return Reject::Leak;
    return Reject::Ok;
}

Reject Controller::request(const Request& r) {
    Reject res = check(r);
    if (res != Reject::Ok) return res;
    if (qCount_ >= QUEUE_LEN) return Reject::QueueFull;
    queue_[(qHead_ + qCount_) % QUEUE_LEN] = r;
    qCount_++;
    return Reject::Ok;
}

void Controller::stopAll() {
    while (qCount_) {
        const Request& r = queue_[qHead_];
        push(EventCode::RunFinished, r.zone, (uint8_t)EndReason::Stopped, 0, 0, r.source);
        qHead_ = (qHead_ + 1) % QUEUE_LEN;
        qCount_--;
    }
    if (active_ && run_.state != State::Settling) {
        uint32_t secs = (nowMs_ - run_.startMs) / 1000;
        push(EventCode::RunFinished, run_.req.zone, (uint8_t)EndReason::Stopped, run_.deliveredMl, secs,
             run_.req.source);
        run_.state = State::Settling;
        run_.closedMs = nowMs_;
    }
}

void Controller::clearFault(uint8_t zone) {
    if (zone == NONE) {
        for (auto& f : fault_) f = 0;
    } else if (zone < MAX_ZONES) {
        fault_[zone] = 0;
    }
}

void Controller::resetDaily() {
    for (auto& d : dailyMl_) d = 0;
}

void Controller::start(const Request& r, const Inputs& in) {
    const ZoneConfig& z = cfg_.zone[r.zone];
    run_ = Run();
    run_.req = r;
    run_.startMs = run_.windowMs = in.nowMs;
    uint32_t limit = r.maxS ? r.maxS : z.maxRunS;
    if (z.flowSensor == NONE && r.targetMl) {
        // No sensor: a volume request becomes a timed run at the nominal flow.
        uint32_t s = (uint32_t)(((uint64_t)r.targetMl * 60 + z.nominalMlPerMin - 1) / z.nominalMlPerMin);
        if (s < limit) limit = s;
    }
    if (limit > z.maxRunS) limit = z.maxRunS;
    run_.limitS = limit;
    if (z.flowSensor != NONE) {
        run_.startFlow = in.flowMl[z.flowSensor];
        lastZone_[z.flowSensor] = r.zone;
        run_.state = State::Priming;
    } else {
        run_.state = State::Dosing;
    }
    active_ = true;
    push(EventCode::RunStarted, r.zone, 0, r.targetMl, limit, r.source);
}

void Controller::end(const Inputs& in, EventCode code, uint8_t detail) {
    uint32_t secs = (in.nowMs - run_.startMs) / 1000;
    if (code == EventCode::RunFault) fault_[run_.req.zone] |= detail;
    push(code, run_.req.zone, detail, run_.deliveredMl, secs, run_.req.source);
    run_.state = State::Settling;
    run_.closedMs = in.nowMs;
}

void Controller::tickRun(const Inputs& in) {
    const uint8_t zi = run_.req.zone;
    const ZoneConfig& z = cfg_.zone[zi];
    const bool sensor = z.flowSensor != NONE;
    const uint32_t elapsedMs = in.nowMs - run_.startMs;

    if (run_.state == State::Settling) {
        if (in.nowMs - run_.closedMs >= (uint32_t)cfg_.settleS * 1000) active_ = false;
        return;
    }

    uint32_t delivered =
        sensor ? in.flowMl[z.flowSensor] - run_.startFlow : (uint32_t)((uint64_t)elapsedMs * z.nominalMlPerMin / 60000);
    dailyMl_[zi] += delivered - run_.deliveredMl;
    run_.deliveredMl = delivered;

    if (in.lockoutMask & (1u << zi)) return end(in, EventCode::RunFinished, (uint8_t)EndReason::Stopped);
    if (sensor && run_.req.targetMl && delivered >= run_.req.targetMl)
        return end(in, EventCode::RunFinished, (uint8_t)EndReason::TargetReached);
    if (delivered >= z.maxRunMl || dailyMl_[zi] >= z.maxDailyMl)
        return end(in, EventCode::RunFinished, (uint8_t)EndReason::VolumeLimit);
    if (elapsedMs >= run_.limitS * 1000) {
        // A sensor zone that reached its time limit without ever priming has no water.
        if (run_.state == State::Priming) return end(in, EventCode::RunFault, FAULT_NO_FLOW);
        return end(in, EventCode::RunFinished, (uint8_t)EndReason::TimeLimit);
    }
    if (!sensor) return;

    if (run_.state == State::Priming) {
        if (delivered >= cfg_.primeMl) {
            run_.state = State::Dosing;
            run_.windowMs = in.nowMs;
            run_.windowMl = delivered;
            run_.lowMs = 0;
        } else if (elapsedMs >= (uint32_t)cfg_.primeTimeoutS * 1000) {
            return end(in, EventCode::RunFault, FAULT_NO_FLOW);
        }
        return;
    }

    // Dosing: judge the flow rate once per window.
    uint32_t winMs = in.nowMs - run_.windowMs;
    if (winMs >= (uint32_t)cfg_.rateWindowS * 1000) {
        uint32_t rate = (uint32_t)((uint64_t)(delivered - run_.windowMl) * 60000 / winMs);
        run_.windowMs = in.nowMs;
        run_.windowMl = delivered;
        if (rate > z.maxMlPerMin) return end(in, EventCode::RunFault, FAULT_BURST);
        if (rate < z.minMlPerMin) {
            run_.lowMs += winMs;
            if (run_.lowMs >= (uint32_t)cfg_.stallS * 1000) return end(in, EventCode::RunFault, FAULT_NO_FLOW);
        } else {
            run_.lowMs = 0;
        }
    }
}

bool Controller::sensorBusy(uint8_t sensor, const Inputs& in) const {
    for (uint8_t i = 0; i < cfg_.zones && i < MAX_ZONES; i++) {
        if (cfg_.zone[i].flowSensor != sensor) continue;
        if (in.manualMask & (1u << i)) return true;
        if (active_ && run_.req.zone == i) return true;  // running or settling
    }
    return false;
}

void Controller::tickLeak(const Inputs& in) {
    for (uint8_t s = 0; s < MAX_FLOW; s++) {
        if (sensorBusy(s, in) || !leakInit_[s]) {
            leakInit_[s] = true;
            leakBaseMl_[s] = in.flowMl[s];
            leakBaseMs_[s] = in.nowMs;
            continue;
        }
        uint32_t ml = in.flowMl[s] - leakBaseMl_[s];
        bool alarm = leakMask_ & (1u << s);
        if (ml >= cfg_.leakMl && !alarm) {
            leakMask_ |= (1u << s);
            push(EventCode::Leak, s, lastZone_[s], ml, (in.nowMs - leakBaseMs_[s]) / 1000);
        }
        if (in.nowMs - leakBaseMs_[s] >= (uint32_t)cfg_.leakWindowS * 1000) {
            if (alarm && ml < cfg_.leakMl) {
                leakMask_ &= ~(1u << s);
                push(EventCode::LeakCleared, s, 0, ml, 0);
            } else if (alarm) {
                push(EventCode::Leak, s, lastZone_[s], ml, cfg_.leakWindowS);  // still leaking: repeat
            }
            leakBaseMl_[s] = in.flowMl[s];
            leakBaseMs_[s] = in.nowMs;
        }
    }
}

void Controller::tick(const Inputs& in) {
    lockoutMask_ = in.lockoutMask;
    nowMs_ = in.nowMs;
    if (active_) tickRun(in);
    tickLeak(in);
    while (!active_ && qCount_) {
        Request r = queue_[qHead_];
        qHead_ = (qHead_ + 1) % QUEUE_LEN;
        qCount_--;
        if (check(r) == Reject::Ok)
            start(r, in);
        else
            push(EventCode::RunFinished, r.zone, (uint8_t)EndReason::Dropped, 0, 0, r.source);
    }
}

uint8_t Controller::valveMask() const {
    if (!active_ || run_.state == State::Settling) return 0;
    uint8_t m = (uint8_t)(1u << run_.req.zone);
    if (cfg_.masterValve != NONE) m |= (uint8_t)(1u << cfg_.masterValve);
    return m;
}

State Controller::state(uint8_t zone) const {
    if (active_ && run_.req.zone == zone) return run_.state;
    for (uint8_t i = 0; i < qCount_; i++)
        if (queue_[(qHead_ + i) % QUEUE_LEN].zone == zone) return State::Queued;
    return State::Idle;
}

void Controller::push(EventCode code, uint8_t zone, uint8_t detail, uint32_t a, uint32_t b, uint8_t source) {
    if (evCount_ == 8) {  // full: drop the oldest
        evHead_ = (evHead_ + 1) % 8;
        evCount_--;
    }
    events_[(evHead_ + evCount_) % 8] = Event{code, zone, detail, a, b, source};
    evCount_++;
}

bool Controller::popEvent(Event& out) {
    if (!evCount_) return false;
    out = events_[evHead_];
    evHead_ = (evHead_ + 1) % 8;
    evCount_--;
    return true;
}

}  // namespace irr

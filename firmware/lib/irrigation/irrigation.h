#pragma once
// Irrigation core: turns watering requests into valve states, one zone at a
// time, and enforces the safety rules. Pure logic (no Arduino dependencies):
// the caller feeds time + cumulative flow readings into tick() and applies
// valveMask() to the outputs. Unit-tested on the host (pio test -e native).
//
// Run:  queued -> PRIMING (flow must appear) -> DOSING (until volume or time)
//       -> SETTLING (valve closed, flow coasts down) -> next request
// Faults close the valve and lock the zone until clearFault().
#include <stddef.h>
#include <stdint.h>

namespace irr {

constexpr uint8_t MAX_ZONES = 8;
constexpr uint8_t MAX_FLOW = 4;
constexpr uint8_t QUEUE_LEN = 8;
constexpr uint8_t NONE = 0xFF;

enum class State : uint8_t { Idle = 0, Queued = 1, Priming = 2, Dosing = 3, Settling = 4 };

enum Fault : uint8_t {
    FAULT_NO_FLOW = 1,  // valve open but no/too little water moving (closed tap, dead valve, clog)
    FAULT_BURST = 2,    // flow far above what the zone can use (burst pipe, popped fitting)
};

enum class EventCode : uint8_t {
    RunStarted = 1,   // a = target ml (0 = timed), b = time limit s
    RunFinished = 2,  // detail = EndReason, a = ml delivered, b = seconds
    RunFault = 3,     // detail = Fault, a = ml delivered, b = seconds
    Leak = 4,         // zone = flow sensor index, detail = last zone run on it, a = ml seen
    LeakCleared = 5,  // zone = flow sensor index
};

enum class EndReason : uint8_t {
    TargetReached = 0,
    TimeLimit = 1,     // timed run finished, or volume run cut by its time limit
    VolumeLimit = 2,   // zone per-run or per-day cap
    Stopped = 3,       // stop command, lockout switch
    Dropped = 4,       // was queued but no longer allowed when its turn came
};

enum class Reject : uint8_t { Ok = 0, BadZone, Disabled, Locked, QueueFull, Invalid, DailyLimit, LockedOut, Leak };

struct ZoneConfig {
    bool enabled = true;
    uint8_t flowSensor = NONE;       // index into Inputs::flowMl, NONE = no sensor (time-based)
    uint16_t maxRunS = 1800;         // hard cap on any single run
    uint32_t maxRunMl = 30000;
    uint32_t maxDailyMl = 120000;
    uint32_t nominalMlPerMin = 1000; // expected flow: converts volume to time when there is no sensor
    uint32_t minMlPerMin = 100;      // below this -> FAULT_NO_FLOW
    uint32_t maxMlPerMin = 6000;     // above this -> FAULT_BURST
};

struct Config {
    uint8_t zones = 0;
    ZoneConfig zone[MAX_ZONES];
    uint8_t masterValve = NONE;   // output index opened together with any zone
    uint16_t primeTimeoutS = 30;  // flow must reach primeMl within this
    uint32_t primeMl = 50;
    uint16_t rateWindowS = 10;    // flow rate is judged over windows of this length
    uint16_t stallS = 30;         // low flow for this long while dosing -> FAULT_NO_FLOW
    uint16_t settleS = 10;        // after closing: flow may coast, next run waits
    uint32_t leakMl = 500;        // this much flow with the valves closed...
    uint16_t leakWindowS = 600;   // ...within this window raises a leak alarm
};

struct Request {
    uint8_t zone = 0;
    uint32_t targetMl = 0;  // 0 = timed run
    uint16_t maxS = 0;      // duration for timed runs, time limit for volume runs (0 = zone default)
    uint8_t source = 0;     // free for the caller (schedule, panel, remote), reported back in events
};

struct Inputs {
    uint32_t nowMs = 0;
    uint32_t flowMl[MAX_FLOW] = {};  // cumulative millilitres per sensor (may wrap)
    uint8_t manualMask = 0;          // zones forced open by hand (HOA switch in HAND)
    uint8_t lockoutMask = 0;         // zones that must not run (HOA switch in OFF)
};

struct Event {
    EventCode code;
    uint8_t zone;
    uint8_t detail;
    uint32_t a;
    uint32_t b;
    uint8_t source;
};

class Controller {
   public:
    explicit Controller(const Config& cfg) : cfg_(cfg) {}

    Config& config() { return cfg_; }

    Reject request(const Request& r);
    void stopAll();  // abort the active run and empty the queue
    void clearFault(uint8_t zone);
    void resetDaily();  // call at local midnight

    void tick(const Inputs& in);

    // Bit i = output i should be open (zones, plus the master valve if configured).
    uint8_t valveMask() const;
    uint8_t activeZone() const { return active_ ? run_.req.zone : NONE; }
    State state(uint8_t zone) const;
    uint8_t fault(uint8_t zone) const { return zone < MAX_ZONES ? fault_[zone] : 0; }
    uint32_t runMl() const { return active_ ? run_.deliveredMl : 0; }
    uint32_t dailyMl(uint8_t zone) const { return zone < MAX_ZONES ? dailyMl_[zone] : 0; }
    uint8_t leakMask() const { return leakMask_; }
    uint8_t queued() const { return qCount_; }
    bool busy() const { return active_ || qCount_; }

    bool popEvent(Event& out);

   private:
    struct Run {
        Request req;
        State state = State::Idle;
        uint32_t startMs = 0;
        uint32_t limitS = 0;
        uint32_t startFlow = 0;
        uint32_t deliveredMl = 0;
        uint32_t windowMs = 0;
        uint32_t windowMl = 0;
        uint32_t lowMs = 0;
        uint32_t closedMs = 0;
    };

    Reject check(const Request& r) const;
    void start(const Request& r, const Inputs& in);
    void tickRun(const Inputs& in);
    void end(const Inputs& in, EventCode code, uint8_t detail);
    void tickLeak(const Inputs& in);
    bool sensorBusy(uint8_t sensor, const Inputs& in) const;
    void push(EventCode code, uint8_t zone, uint8_t detail, uint32_t a, uint32_t b, uint8_t source = 0);

    Config cfg_;
    bool active_ = false;
    Run run_;
    Request queue_[QUEUE_LEN];
    uint8_t qHead_ = 0, qCount_ = 0;
    uint8_t fault_[MAX_ZONES] = {};
    uint32_t dailyMl_[MAX_ZONES] = {};
    uint8_t lockoutMask_ = 0;
    uint32_t nowMs_ = 0;  // time of the last tick()

    // leak monitor, per flow sensor
    bool leakInit_[MAX_FLOW] = {};
    uint32_t leakBaseMl_[MAX_FLOW] = {};
    uint32_t leakBaseMs_[MAX_FLOW] = {};
    uint8_t lastZone_[MAX_FLOW] = {NONE, NONE, NONE, NONE};
    uint8_t leakMask_ = 0;

    Event events_[8];
    uint8_t evHead_ = 0, evCount_ = 0;
};

}  // namespace irr

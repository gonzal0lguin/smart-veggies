// Host unit tests: irrigation core against a simulated plumbing model.
//   pio test -e native
#include <unity.h>

#include "irrigation.h"

using namespace irr;

void setUp() {}
void tearDown() {}

// 6 zones in 3 beds: zone z is measured by sensor z/2 (as on the real system).
struct Rig {
    Controller c;
    Inputs in;
    double acc[MAX_FLOW] = {};
    uint32_t offset = 0;            // added to every flow counter (wrap tests)
    uint32_t rate[MAX_ZONES];       // ml/min through a zone when its valve is open
    uint32_t leak[MAX_FLOW] = {};   // ml/min flowing with everything closed
    int maxOpen = 0;
    Event ev[64];
    int nev = 0;

    static Config makeConfig() {
        Config cfg;
        cfg.zones = 6;
        for (uint8_t z = 0; z < 6; z++) cfg.zone[z].flowSensor = z / 2;
        return cfg;
    }
    explicit Rig(const Config& cfg = makeConfig()) : c(cfg) {
        for (auto& r : rate) r = 1000;
    }

    void step(uint32_t dtMs = 100) {
        uint8_t mask = c.valveMask() | in.manualMask;
        int open = 0;
        for (uint8_t z = 0; z < c.config().zones; z++) {
            if (!(mask & (1u << z))) continue;
            if (c.valveMask() & (1u << z)) open++;
            uint8_t s = c.config().zone[z].flowSensor;
            if (s != NONE) acc[s] += rate[z] * (dtMs / 60000.0);
        }
        if (open > maxOpen) maxOpen = open;
        for (uint8_t s = 0; s < MAX_FLOW; s++) {
            acc[s] += leak[s] * (dtMs / 60000.0);
            in.flowMl[s] = (uint32_t)acc[s] + offset;
        }
        in.nowMs += dtMs;
        c.tick(in);
        Event e;
        while (c.popEvent(e))
            if (nev < 64) ev[nev++] = e;
    }
    void run(uint32_t seconds) {
        for (uint32_t i = 0; i < seconds * 10; i++) step();
    }
    const Event* find(EventCode code, uint8_t zone, int nth = 0) const {
        for (int i = 0; i < nev; i++)
            if (ev[i].code == code && ev[i].zone == zone && nth-- == 0) return &ev[i];
        return nullptr;
    }
};

static Request vol(uint8_t zone, uint32_t ml, uint16_t maxS = 0) {
    Request r;
    r.zone = zone;
    r.targetMl = ml;
    r.maxS = maxS;
    return r;
}
static Request timed(uint8_t zone, uint16_t s) { return vol(zone, 0, s); }

static void test_volume_dose() {
    Rig r;
    r.run(5);
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(0, 4000)));
    r.step();
    TEST_ASSERT_EQUAL(0x01, r.c.valveMask());
    TEST_ASSERT_EQUAL((int)State::Priming, (int)r.c.state(0));
    r.run(10);
    TEST_ASSERT_EQUAL((int)State::Dosing, (int)r.c.state(0));
    r.run(300);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
    TEST_ASSERT_FALSE(r.c.busy());
    const Event* fin = r.find(EventCode::RunFinished, 0);
    TEST_ASSERT_NOT_NULL(r.find(EventCode::RunStarted, 0));
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_EQUAL((int)EndReason::TargetReached, fin->detail);
    TEST_ASSERT_UINT32_WITHIN(5, 4000, fin->a);
    TEST_ASSERT_UINT32_WITHIN(2, 240, fin->b);
    TEST_ASSERT_UINT32_WITHIN(5, 4000, r.c.dailyMl(0));
    TEST_ASSERT_EQUAL(0, r.c.fault(0));
}

static void test_timed_run_without_sensor() {
    Config cfg = Rig::makeConfig();
    cfg.zone[4].flowSensor = NONE;
    Rig r(cfg);
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(timed(4, 60)));
    r.run(59);
    TEST_ASSERT_EQUAL(0x10, r.c.valveMask());
    r.run(3);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
    const Event* fin = r.find(EventCode::RunFinished, 4);
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_EQUAL((int)EndReason::TimeLimit, fin->detail);
    TEST_ASSERT_UINT32_WITHIN(20, 1000, fin->a);  // estimated from the nominal flow
    // a volume request on a sensorless zone becomes time at nominal flow
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(4, 2000)));
    r.run(15);  // previous run is still settling for part of this
    r.run(100);
    TEST_ASSERT_EQUAL(0x10, r.c.valveMask());
    r.run(20);  // 2 L at 1 L/min = 120 s
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
}

static void test_no_flow_faults_and_locks() {
    Rig r;
    r.rate[2] = 0;  // tap closed / dead valve
    r.c.request(vol(2, 4000));
    r.run(29);
    TEST_ASSERT_EQUAL(0x04, r.c.valveMask());
    r.run(2);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
    const Event* f = r.find(EventCode::RunFault, 2);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(FAULT_NO_FLOW, f->detail);
    TEST_ASSERT_EQUAL(FAULT_NO_FLOW, r.c.fault(2));
    TEST_ASSERT_EQUAL((int)Reject::Locked, (int)r.c.request(vol(2, 1000)));
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(3, 1000)));  // sibling zone unaffected
    r.c.clearFault(2);
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(2, 1000)));
}

static void test_flow_stalls_mid_run() {
    Rig r;
    r.c.request(vol(0, 20000));
    r.run(60);
    r.rate[0] = 0;  // supply lost
    r.run(25);
    TEST_ASSERT_EQUAL(0x01, r.c.valveMask());  // short dips are tolerated...
    r.run(20);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());     // ...stallS (30 s) of no flow is not
    TEST_ASSERT_EQUAL(FAULT_NO_FLOW, r.c.fault(0));
}

static void test_burst_closes_fast() {
    Rig r;
    r.rate[1] = 15000;  // pipe popped off
    r.c.request(vol(1, 20000));
    r.run(13);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
    const Event* f = r.find(EventCode::RunFault, 1);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(FAULT_BURST, f->detail);
    TEST_ASSERT_TRUE(f->a < 4000);
}

static void test_queue_runs_one_zone_at_a_time() {
    Rig r;
    for (uint8_t z = 0; z < 6; z++) TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(z, 1000)));
    TEST_ASSERT_EQUAL((int)State::Queued, (int)r.c.state(5));
    r.run(6 * 75);
    TEST_ASSERT_FALSE(r.c.busy());
    TEST_ASSERT_EQUAL(1, r.maxOpen);
    for (uint8_t z = 0; z < 6; z++) {
        const Event* fin = r.find(EventCode::RunFinished, z);
        TEST_ASSERT_NOT_NULL(fin);
        TEST_ASSERT_UINT32_WITHIN(5, 1000, fin->a);  // neighbour's coasting is not counted
    }
    TEST_ASSERT_EQUAL(0, r.c.leakMask());
}

static void test_queue_full() {
    Rig r;
    for (int i = 0; i < QUEUE_LEN; i++) TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(0, 100)));
    TEST_ASSERT_EQUAL((int)Reject::QueueFull, (int)r.c.request(vol(0, 100)));
}

static void test_time_limit_caps_volume_run() {
    Config cfg = Rig::makeConfig();
    cfg.zone[0].maxRunS = 120;
    Rig r(cfg);
    r.c.request(vol(0, 20000, 600));  // asks for more time than the zone allows
    r.run(119);
    TEST_ASSERT_EQUAL(0x01, r.c.valveMask());
    r.run(2);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
    TEST_ASSERT_EQUAL((int)EndReason::TimeLimit, r.find(EventCode::RunFinished, 0)->detail);
    TEST_ASSERT_EQUAL(0, r.c.fault(0));
}

static void test_volume_caps() {
    Config cfg = Rig::makeConfig();
    cfg.zone[0].maxRunMl = 1500;
    cfg.zone[1].maxDailyMl = 2500;
    Rig r(cfg);
    r.c.request(vol(0, 5000));
    r.run(120);
    TEST_ASSERT_EQUAL((int)EndReason::VolumeLimit, r.find(EventCode::RunFinished, 0)->detail);
    TEST_ASSERT_UINT32_WITHIN(5, 1500, r.find(EventCode::RunFinished, 0)->a);

    r.c.request(vol(1, 2000));
    r.run(150);
    r.c.request(vol(1, 2000));  // only 500 ml left today
    r.run(150);
    TEST_ASSERT_EQUAL((int)EndReason::VolumeLimit, r.find(EventCode::RunFinished, 1, 1)->detail);
    TEST_ASSERT_UINT32_WITHIN(5, 2500, r.c.dailyMl(1));
    TEST_ASSERT_EQUAL((int)Reject::DailyLimit, (int)r.c.request(vol(1, 100)));
    r.c.resetDaily();
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(1, 100)));
}

static void test_stop_all() {
    Rig r;
    r.c.request(vol(0, 5000));
    r.c.request(vol(3, 5000));
    r.run(30);
    r.c.stopAll();
    TEST_ASSERT_EQUAL(0, r.c.valveMask());  // immediately, before the next tick
    r.run(20);
    TEST_ASSERT_FALSE(r.c.busy());
    TEST_ASSERT_EQUAL((int)EndReason::Stopped, r.find(EventCode::RunFinished, 0)->detail);
    TEST_ASSERT_UINT32_WITHIN(20, 500, r.find(EventCode::RunFinished, 0)->a);
    TEST_ASSERT_EQUAL((int)EndReason::Stopped, r.find(EventCode::RunFinished, 3)->detail);
    TEST_ASSERT_NULL(r.find(EventCode::RunStarted, 3));
}

static void test_bad_requests() {
    Rig r;
    TEST_ASSERT_EQUAL((int)Reject::BadZone, (int)r.c.request(vol(6, 100)));
    TEST_ASSERT_EQUAL((int)Reject::Invalid, (int)r.c.request(vol(0, 0, 0)));
    r.c.config().zone[1].enabled = false;
    TEST_ASSERT_EQUAL((int)Reject::Disabled, (int)r.c.request(vol(1, 100)));
}

static void test_leak_with_valves_closed() {
    Rig r;
    r.run(20);
    r.leak[0] = 1000;  // valve of bed 1 stuck open / pipe leak upstream
    r.run(35);
    const Event* l = r.find(EventCode::Leak, 0);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL(0x01, r.c.leakMask());
    TEST_ASSERT_EQUAL((int)Reject::Leak, (int)r.c.request(vol(1, 100)));  // same sensor
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(2, 100)));    // other bed
    r.leak[0] = 0;
    r.run(1300);
    TEST_ASSERT_EQUAL(0, r.c.leakMask());
    TEST_ASSERT_NOT_NULL(r.find(EventCode::LeakCleared, 0));
    TEST_ASSERT_EQUAL((int)Reject::Ok, (int)r.c.request(vol(1, 100)));
}

static void test_slow_drip_below_threshold_is_not_a_leak() {
    Rig r;
    r.leak[1] = 30;  // 300 ml per 10 min window < 500 ml
    r.run(3600);
    TEST_ASSERT_EQUAL(0, r.c.leakMask());
}

static void test_hand_mode_flow_is_not_a_leak() {
    Rig r;
    r.in.manualMask = 0x04;  // zone 2 switched to HAND
    r.run(600);
    TEST_ASSERT_EQUAL(0, r.c.leakMask());
    r.in.manualMask = 0;
    r.run(700);
    TEST_ASSERT_EQUAL(0, r.c.leakMask());
}

static void test_lockout_switch() {
    Rig r;
    r.c.request(vol(0, 5000));
    r.run(30);
    r.in.lockoutMask = 0x01;  // HOA switch to OFF
    r.step();
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
    TEST_ASSERT_EQUAL((int)EndReason::Stopped, r.find(EventCode::RunFinished, 0)->detail);
    TEST_ASSERT_EQUAL((int)Reject::LockedOut, (int)r.c.request(vol(0, 100)));
    // queued before the switch moved: dropped when its turn comes
    r.in.lockoutMask = 0;
    r.run(15);
    r.c.request(vol(1, 5000));
    r.c.request(vol(0, 100));
    r.in.lockoutMask = 0x01;
    r.run(400);
    TEST_ASSERT_EQUAL((int)EndReason::Dropped, r.find(EventCode::RunFinished, 0, 1)->detail);
}

static void test_master_valve_follows() {
    Config cfg = Rig::makeConfig();
    cfg.zones = 5;
    cfg.masterValve = 5;
    Rig r(cfg);
    TEST_ASSERT_EQUAL((int)Reject::BadZone, (int)r.c.request(vol(5, 100)));
    r.c.request(vol(2, 1000));
    r.run(5);
    TEST_ASSERT_EQUAL(0x24, r.c.valveMask());
    r.run(80);
    TEST_ASSERT_EQUAL(0, r.c.valveMask());
}

static void test_counter_and_clock_wraparound() {
    Rig r;
    r.offset = 0xFFFFFFFFu - 1500;  // flow counter wraps mid-run
    r.in.nowMs = 0xFFFFFFFFu - 60000;  // millis() wraps mid-run
    r.step();
    r.c.request(vol(0, 4000));
    r.run(300);
    const Event* fin = r.find(EventCode::RunFinished, 0);
    TEST_ASSERT_NOT_NULL(fin);
    TEST_ASSERT_EQUAL((int)EndReason::TargetReached, fin->detail);
    TEST_ASSERT_UINT32_WITHIN(5, 4000, fin->a);
    TEST_ASSERT_UINT32_WITHIN(2, 240, fin->b);
    TEST_ASSERT_EQUAL(0, r.c.leakMask());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_volume_dose);
    RUN_TEST(test_timed_run_without_sensor);
    RUN_TEST(test_no_flow_faults_and_locks);
    RUN_TEST(test_flow_stalls_mid_run);
    RUN_TEST(test_burst_closes_fast);
    RUN_TEST(test_queue_runs_one_zone_at_a_time);
    RUN_TEST(test_queue_full);
    RUN_TEST(test_time_limit_caps_volume_run);
    RUN_TEST(test_volume_caps);
    RUN_TEST(test_stop_all);
    RUN_TEST(test_bad_requests);
    RUN_TEST(test_leak_with_valves_closed);
    RUN_TEST(test_slow_drip_below_threshold_is_not_a_leak);
    RUN_TEST(test_hand_mode_flow_is_not_a_leak);
    RUN_TEST(test_lockout_switch);
    RUN_TEST(test_master_valve_follows);
    RUN_TEST(test_counter_and_clock_wraparound);
    return UNITY_END();
}

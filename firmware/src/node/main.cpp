// Field node application: sensors -> irrigation core -> valves, schedules,
// LoRa telemetry/events and authenticated remote commands, serial console.
//
//   pio run -e node -t upload -t monitor       real hardware
//   pio run -e node-sim -t upload -t monitor   bare ESP32, simulated flow + soil
//
// The node waters on its own (schedules + safety). The link only reports and
// accepts intents. Type `help` on the serial console.
#include <Arduino.h>
#include <DHTesp.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "e32.h"
#include "flow_meter.h"
#include "irrigation.h"
#include "link.h"
#include "messages.h"
#include "scheduler.h"
#include "serial_cmd.h"
#include "soil_moisture.h"
#include "valves.h"

enum Source : uint8_t { SRC_CONSOLE = 1, SRC_SCHEDULE = 2, SRC_REMOTE = 3 };

// ------------------------------------------------------------------ hardware
static const int8_t valvePins[] = VALVE_PINS;
static const uint8_t flowPins[] = FLOW_PINS;
static const uint8_t zoneFlowSensor[] = ZONE_FLOW_SENSOR;
static const uint8_t soilZone[] = SOIL_CHANNEL_ZONE;
static const uint8_t adsAddresses[] = SOIL_ADS_ADDRESSES;
static const uint8_t adcPins[] = SOIL_FALLBACK_ADC_PINS;
static const uint8_t linkKey[] = LINK_KEY;
static const size_t linkKeyLen = sizeof(linkKey) - 1;

static ValveBank valves(valvePins, VALVE_COUNT, VALVE_RELAY_ACTIVE_LOW);
static FlowMeter flow[FLOW_COUNT];
static SoilMoisture soil;
static OneWire oneWire(PIN_ONEWIRE);
static DallasTemperature probes(&oneWire);
static DHTesp dht;
static E32 radio(Serial2, PIN_LORA_RX, PIN_LORA_TX, PIN_LORA_MODE, PIN_LORA_AUX);
static LoraLink lora(radio, NODE_ADDRESS);
static Preferences prefs;
static LineReader console;

static irr::Config makeIrrConfig() {
    irr::Config c;
    c.zones = VALVE_COUNT;
    for (uint8_t z = 0; z < VALVE_COUNT; z++) {
        irr::ZoneConfig& zc = c.zone[z];
        zc.flowSensor = zoneFlowSensor[z] < FLOW_COUNT ? zoneFlowSensor[z] : irr::NONE;
        zc.maxRunS = ZONE_MAX_RUN_S;
        zc.maxRunMl = ZONE_MAX_RUN_L * 1000UL;
        zc.maxDailyMl = ZONE_MAX_DAILY_L * 1000UL;
        zc.nominalMlPerMin = ZONE_NOMINAL_ML_MIN;
        zc.minMlPerMin = ZONE_MIN_ML_MIN;
        zc.maxMlPerMin = ZONE_MAX_ML_MIN;
    }
    return c;
}
static irr::Controller ctl(makeIrrConfig());
static sched::Scheduler schedule;

// --------------------------------------------------------------------- state
static bool radioOk = false;
static bool autoMode = true;
static uint32_t cmdCounter = 0;       // last accepted authenticated counter (persisted)
static uint8_t lastCmdSeq = 0;
static proto::Ack lastCmdAck;         // re-sent when the gateway retries the same command
static uint16_t eventId = 0;
static uint32_t rainSkipUntilS = 0;   // uptime seconds; schedules suspended until then
static bool statusDue = true;

static float soilPct[proto::SOIL_CHANNELS];
static float soilMv[proto::SOIL_CHANNELS];
static float soilTemp[SOIL_TEMP_COUNT];
static float airTemp = NAN, airRh = NAN;

// Wall clock: set by the gateway (TIME) or the console. No RTC yet (Phase 3:
// DS3231), so after a power cut schedules wait until the first time sync.
static struct {
    bool valid = false;
    uint32_t baseEpoch = 0, baseMs = 0;
    int16_t tzMin = 0;
    void set(uint32_t epoch) {
        baseEpoch = epoch;
        baseMs = millis();
        valid = true;
    }
    uint32_t utc() {
        if (!valid) return 0;
        uint32_t el = millis() - baseMs;
        if (el >= 3600000UL) {  // re-base hourly so millis() wrap never matters
            baseEpoch += el / 1000;
            baseMs += el / 1000 * 1000;
            el = millis() - baseMs;
        }
        return baseEpoch + el / 1000;
    }
    uint32_t local() { return valid ? utc() + (int32_t)tzMin * 60 : 0; }
} wallClock;

static uint32_t uptimeS() {
    static uint32_t lastMs = 0, secs = 0, frac = 0;
    uint32_t now = millis();
    frac += now - lastMs;
    lastMs = now;
    secs += frac / 1000;
    frac %= 1000;
    return secs;
}

// ----------------------------------------------------------- sensors / sim
#ifdef SIM
static double simFlowMl[FLOW_COUNT];
static uint32_t simRate[VALVE_COUNT];  // ml/min when open (0 = simulate a dead zone)
static uint32_t simLastMs = 0;

static void simBegin() {
    for (auto& r : simRate) r = ZONE_NOMINAL_ML_MIN;
    for (uint8_t i = 0; i < proto::SOIL_CHANNELS; i++) soilPct[i] = 35.0f + 3.0f * i;
}
static void simTick() {
    uint32_t now = millis(), dt = now - simLastMs;
    if (dt < 100) return;
    simLastMs = now;
    for (uint8_t z = 0; z < VALVE_COUNT; z++) {
        bool open = valves.isOpen(z);
        if (open && zoneFlowSensor[z] < FLOW_COUNT) simFlowMl[zoneFlowSensor[z]] += simRate[z] * (dt / 60000.0);
        for (uint8_t ch = 0; ch < proto::SOIL_CHANNELS; ch++) {
            if (soilZone[ch] != z) continue;
            // dries ~2 %/h, wets ~4 %/min while watered
            soilPct[ch] += (open && simRate[z] ? 4.0f : -0.033f) * (dt / 60000.0f);
            soilPct[ch] = constrain(soilPct[ch], 5.0f, 95.0f);
        }
    }
}
static uint32_t flowMl(uint8_t s) { return (uint32_t)simFlowMl[s]; }
static void readSensors() {
    for (uint8_t i = 0; i < SOIL_TEMP_COUNT; i++) soilTemp[i] = 18.0f + i;
    airTemp = 22.0f + 5.0f * sinf(millis() / 600000.0f);
    airRh = 55.0f;
}
#else
static uint32_t flowMl(uint8_t s) { return (uint32_t)((uint64_t)flow[s].pulses() * 1000 / flow[s].pulsesPerLitre()); }

static void loadCalibration() {
    for (uint8_t ch = 0; ch < soil.channels(); ch++) {
        char key[8];
        float v[2] = {0, 0};
        snprintf(key, sizeof(key), "cal%u", ch);
        if (prefs.getBytes(key, v, sizeof(v)) == sizeof(v)) {
            soil.calibration(ch).dryMv = v[0];
            soil.calibration(ch).wetMv = v[1];
        }
    }
    for (uint8_t s = 0; s < FLOW_COUNT; s++) {
        char key[8];
        snprintf(key, sizeof(key), "k%u", s);
        flow[s].setPulsesPerLitre(prefs.getFloat(key, FLOW_PULSES_PER_LITRE));
    }
}

// Soil + air every call; DS18B20 conversion is started here and read next time.
static void readSensors() {
    for (uint8_t ch = 0; ch < proto::SOIL_CHANNELS; ch++) {
        soilPct[ch] = soilMv[ch] = NAN;
        float mv;
        if (ch < soil.channels() && soil.readMilliVolts(ch, mv)) {
            soilMv[ch] = mv;
            soilPct[ch] = soil.toPercent(ch, mv);
        }
    }
    for (uint8_t i = 0; i < SOIL_TEMP_COUNT; i++) {
        float t = i < probes.getDeviceCount() ? probes.getTempCByIndex(i) : DEVICE_DISCONNECTED_C;
        soilTemp[i] = t == DEVICE_DISCONNECTED_C || t == 85.0f ? NAN : t;
    }
    probes.requestTemperatures();  // non-blocking, ready well before the next period
    TempAndHumidity air = dht.getTempAndHumidity();
    bool ok = dht.getStatus() == DHTesp::ERROR_NONE;
    airTemp = ok ? air.temperature : NAN;
    airRh = ok ? air.humidity : NAN;
}
#endif

// Mean moisture of a zone's calibrated sensors, NAN if it has none.
static float zoneMoisture(uint8_t zone) {
    float sum = 0;
    int n = 0;
    for (uint8_t ch = 0; ch < proto::SOIL_CHANNELS; ch++)
        if (soilZone[ch] == zone && !isnan(soilPct[ch])) sum += soilPct[ch], n++;
    return n ? sum / n : NAN;
}

// ---------------------------------------------------------------------- link
static void sendTelemetry() {
    proto::Telemetry t;
    t.epoch = wallClock.utc();
    for (uint8_t ch = 0; ch < proto::SOIL_CHANNELS; ch++)
        if (!isnan(soilPct[ch])) t.soilHalfPct[ch] = (uint8_t)lroundf(constrain(soilPct[ch], 0.0f, 100.0f) * 2);
    for (uint8_t i = 0; i < proto::SOIL_TEMPS && i < SOIL_TEMP_COUNT; i++)
        if (!isnan(soilTemp[i])) t.soilTempC100[i] = (int16_t)lroundf(soilTemp[i] * 100);
    if (!isnan(airTemp)) t.airTempC100 = (int16_t)lroundf(airTemp * 100);
    if (!isnan(airRh)) t.airRhHalfPct = (uint8_t)lroundf(constrain(airRh, 0.0f, 100.0f) * 2);
    for (uint8_t z = 0; z < proto::ZONES && z < VALVE_COUNT; z++)
        t.todayDl[z] = (uint16_t)min<uint32_t>(ctl.dailyMl(z) / 100, 0xFFFF);
    proto::Frame f = lora.make(proto::ADDR_GATEWAY);
    proto::encode(t, f);
    lora.send(f);
}

static uint8_t statusFlags() {
    uint8_t fl = 0;
    if (wallClock.valid) fl |= proto::ST_CLOCK_VALID;
    if (autoMode) fl |= proto::ST_AUTO;
    if (ctl.leakMask()) fl |= proto::ST_LEAK;
    if (uptimeS() < rainSkipUntilS) fl |= proto::ST_RAIN_SKIP;
#ifdef SIM
    fl |= proto::ST_SIM;
#endif
    return fl;
}

static void sendStatus() {
    proto::Status s;
    s.epoch = wallClock.utc();
    s.uptimeS = uptimeS();
    s.flags = statusFlags();
    s.activeZone = ctl.activeZone();
    for (uint8_t z = 0; z < proto::ZONES && z < VALVE_COUNT; z++) {
        s.zoneState[z] = (uint8_t)ctl.state(z);
        s.zoneFault[z] = ctl.fault(z);
        if (valves.isOpen(z)) s.valveMask |= 1u << z;
    }
    s.runMl = ctl.runMl();
    s.cmdCounter = cmdCounter;
    s.resetReason = (uint8_t)esp_reset_reason();
    proto::Frame f = lora.make(proto::ADDR_GATEWAY);
    proto::encode(s, f);
    lora.send(f);
}

static void sendEvent(uint8_t code, uint8_t zone, uint8_t detail, uint32_t a, uint32_t b) {
    proto::Event e;
    e.epoch = wallClock.utc();
    e.id = ++eventId;
    e.code = code;
    e.zone = zone;
    e.detail = detail;
    e.a = a;
    e.b = b;
    proto::Frame f = lora.make(proto::ADDR_GATEWAY);
    proto::encode(e, f);
    if (!lora.sendReliable(f)) Serial.println("link queue full: event not sent");
    statusDue = true;
}

// Executes a verified, fresh command. Returns the ack result; `detail` gets irr::Reject.
static uint8_t execute(const proto::Cmd& c, uint8_t& detail) {
    irr::Request r;
    r.zone = c.zone;
    r.source = SRC_REMOTE;
    switch (c.cmd) {
        case proto::CMD_WATER_VOLUME:
            r.targetMl = c.arg * 100UL;
            r.maxS = c.arg2;
            if (!r.targetMl) return proto::ACK_INVALID;
            detail = (uint8_t)ctl.request(r);
            return detail ? proto::ACK_REJECTED : proto::ACK_OK;
        case proto::CMD_WATER_TIME:
            r.maxS = c.arg;
            detail = (uint8_t)ctl.request(r);
            return detail ? proto::ACK_REJECTED : proto::ACK_OK;
        case proto::CMD_STOP_ALL:
            ctl.stopAll();
            valves.closeAll();
            return proto::ACK_OK;
        case proto::CMD_CLEAR_FAULT:
            ctl.clearFault(c.zone);
            return proto::ACK_OK;
        case proto::CMD_SET_AUTO:
            autoMode = c.arg != 0;
            prefs.putBool("auto", autoMode);
            return proto::ACK_OK;
        case proto::CMD_RAIN_SKIP:
            rainSkipUntilS = c.arg ? uptimeS() + c.arg * 3600UL : 0;
            return proto::ACK_OK;
    }
    return proto::ACK_INVALID;
}

static void onAuthenticated(proto::Frame& f) {
    const uint8_t seq = f.seq;
    if (!proto::verify(f, linkKey, linkKeyLen)) {
        Serial.println("link: bad MAC, ignored");
        return lora.sendAck(f, proto::ACK_BAD_AUTH, 0, cmdCounter);
    }
    proto::Cmd c;
    proto::TimeSync t;
    bool isCmd = proto::decode(f, c), isTime = !isCmd && proto::decode(f, t);
    if (!isCmd && !isTime) return lora.sendAck(f, proto::ACK_INVALID, 0, cmdCounter);
    uint32_t counter = isCmd ? c.counter : t.counter;

    if (counter == cmdCounter && seq == lastCmdSeq && lastCmdAck.type == f.type)  // retry: our ACK was lost
        return lora.sendAck(f, lastCmdAck.result, lastCmdAck.detail, cmdCounter);
    if (counter <= cmdCounter) return lora.sendAck(f, proto::ACK_REPLAY, 0, cmdCounter);

    cmdCounter = counter;
    prefs.putUInt("counter", cmdCounter);
    uint8_t result = proto::ACK_OK, detail = 0;
    if (isTime) {
        wallClock.tzMin = t.tzMinutes;
        wallClock.set(t.epoch);
        Serial.printf("clock set: %u (tz %+d min)\n", t.epoch, t.tzMinutes);
    } else if (c.expiry && wallClock.valid && wallClock.utc() > c.expiry) {
        result = proto::ACK_EXPIRED;
    } else {
        result = execute(c, detail);
        Serial.printf("remote cmd %u zone %u arg %u -> result %u/%u\n", c.cmd, c.zone, c.arg, result, detail);
    }
    lastCmdSeq = seq;
    lastCmdAck.type = f.type;
    lastCmdAck.result = result;
    lastCmdAck.detail = detail;
    lora.sendAck(f, result, detail, cmdCounter);
    statusDue = true;
}

static void pollLink() {
    if (!radioOk) return;
    proto::Frame f;
    while (lora.poll(f)) {
        if (f.src != proto::ADDR_GATEWAY) continue;
        if (f.type == proto::MSG_CMD || f.type == proto::MSG_TIME)
            onAuthenticated(f);
        else if (f.type == proto::MSG_PING) {
            f.dst = f.src;
            f.src = NODE_ADDRESS;
            f.type = proto::MSG_PONG;
            lora.send(f);
        }
    }
    LoraLink::Result r;
    while (lora.popResult(r))
        if (!r.acked) Serial.printf("link: frame type 0x%02X seq %u not acknowledged\n", r.frame.type, r.frame.seq);
}

// ------------------------------------------------------------------ schedules
static void saveSchedule() { prefs.putBytes("progs", schedule.programs, sizeof(schedule.programs)); }

static void runSchedules() {
    if (!wallClock.valid) return;
    uint32_t local = wallClock.local();
    if (schedule.newDay(local)) ctl.resetDaily();
    uint8_t due[sched::MAX_PROGRAMS];
    uint8_t n = schedule.due(local, due, sizeof(due));
    for (uint8_t i = 0; i < n; i++) {
        const sched::Program& p = schedule.programs[due[i]];
        float moist = zoneMoisture(p.zone);
        const char* skip = !autoMode                                                 ? "auto is off"
                           : uptimeS() < rainSkipUntilS                              ? "rain skip"
                           : (p.skipAbovePct && !isnan(moist) && moist >= p.skipAbovePct) ? "soil already wet"
                                                                                     : nullptr;
        if (skip) {
            Serial.printf("program %u (zone %u) skipped: %s\n", due[i] + 1, p.zone + 1, skip);
            continue;
        }
        irr::Request r;
        r.zone = p.zone;
        r.targetMl = p.ml;
        r.maxS = p.seconds;
        r.source = SRC_SCHEDULE;
        irr::Reject res = ctl.request(r);
        Serial.printf("program %u -> zone %u: %s\n", due[i] + 1, p.zone + 1, res == irr::Reject::Ok ? "queued" : "REJECTED");
    }
}

// -------------------------------------------------------------------- console
static const char* stateName(irr::State s) {
    static const char* n[] = {"idle", "queued", "priming", "dosing", "settling"};
    return n[(uint8_t)s];
}

static void printStatus() {
    Serial.printf("\nuptime %us  clock %s (utc %u, tz %+d)  auto %s  radio %s  link tx %u rx %u retries %u failed %u\n",
                  uptimeS(), wallClock.valid ? "ok" : "NOT SET", wallClock.utc(), wallClock.tzMin, autoMode ? "on" : "off",
                  radioOk ? "ok" : "MISSING", lora.txCount, lora.rxCount, lora.retries, lora.failed);
    for (uint8_t z = 0; z < VALVE_COUNT; z++) {
        float m = zoneMoisture(z);
        Serial.printf("  zone %u: %-8s valve %s  fault 0x%02X  today %.1f L  soil %s%.0f %%\n", z + 1, stateName(ctl.state(z)),
                      valves.isOpen(z) ? "OPEN" : "shut", ctl.fault(z), ctl.dailyMl(z) / 1000.0f, isnan(m) ? "n/a " : "",
                      isnan(m) ? 0.0f : m);
    }
    for (uint8_t s = 0; s < FLOW_COUNT; s++) Serial.printf("  flow %u: %.2f L total%s\n", s + 1, flowMl(s) / 1000.0f,
                                                           ctl.leakMask() & (1u << s) ? "  LEAK ALARM" : "");
    Serial.printf("  air %.1f C %.0f %%RH   soil temp %.1f / %.1f C   run %.2f L\n", airTemp, airRh, soilTemp[0], soilTemp[1],
                  ctl.runMl() / 1000.0f);
}

static void printPrograms() {
    for (uint8_t i = 0; i < sched::MAX_PROGRAMS; i++) {
        const sched::Program& p = schedule.programs[i];
        if (!p.enabled) continue;
        Serial.printf("  prog %u: zone %u at %02u:%02u days 0x%02X  %.1f L / %u s  skip above %u %%\n", i + 1, p.zone + 1,
                      p.startMin / 60, p.startMin % 60, p.days, p.ml / 1000.0f, p.seconds, p.skipAbovePct);
    }
}

static void help() {
    Serial.println("commands (zones and sensors are numbered from 1):");
    Serial.println("  status | sensors | progs");
    Serial.println("  water <zone> <litres> [max_s]     dose by volume");
    Serial.println("  run <zone> <seconds>              timed run");
    Serial.println("  stop                              close everything, empty the queue");
    Serial.println("  clear [zone]                      clear fault lock (all zones if omitted)");
    Serial.println("  auto on|off                       enable/disable schedules");
    Serial.println("  time <utc_epoch> [tz_minutes]     set the clock by hand");
    Serial.println("  prog <n> <zone> <HH:MM> <litres> [days_hex=7F] [skip_above_pct=0]");
    Serial.println("  prog <n> off");
#ifdef SIM
    Serial.println("  sim <zone> <ml_per_min>           simulated flow of a zone (0 = dead zone)");
#else
    Serial.println("  cal dry|wet                       store moisture calibration point (all connected sensors)");
    Serial.println("  k <sensor> <pulses_per_litre>     flow sensor calibration");
#endif
}

static void handleLine(const char* line) {
    char cmd[12] = "", s1[12] = "", s2[12] = "";
    float a = 0, b = 0, c = 0;
    int n = sscanf(line, "%11s %11s %11s %f %f %f", cmd, s1, s2, &a, &b, &c);
    int i1 = atoi(s1);
    float f2 = atof(s2);
    bool zoneOk = i1 >= 1 && i1 <= VALVE_COUNT;
    irr::Request r;
    r.zone = (uint8_t)(i1 - 1);
    r.source = SRC_CONSOLE;

    if (!strcmp(cmd, "status")) {
        printStatus();
    } else if (!strcmp(cmd, "sensors")) {
        for (uint8_t ch = 0; ch < proto::SOIL_CHANNELS; ch++)
            Serial.printf("  soil %2u (zone %u): %7.0f mV  %5.1f %%\n", ch + 1, soilZone[ch] + 1, soilMv[ch], soilPct[ch]);
    } else if (!strcmp(cmd, "progs")) {
        printPrograms();
    } else if (!strcmp(cmd, "water") && zoneOk && n >= 3 && f2 > 0) {
        r.targetMl = (uint32_t)(f2 * 1000);
        r.maxS = n >= 4 ? (uint16_t)a : 0;
        Serial.printf("water zone %d %.1f L: reject code %u\n", i1, f2, (uint8_t)ctl.request(r));
    } else if (!strcmp(cmd, "run") && zoneOk && n >= 3 && f2 >= 1) {
        r.maxS = (uint16_t)f2;
        Serial.printf("run zone %d %u s: reject code %u\n", i1, r.maxS, (uint8_t)ctl.request(r));
    } else if (!strcmp(cmd, "stop")) {
        ctl.stopAll();
        valves.closeAll();
        Serial.println("stopped");
    } else if (!strcmp(cmd, "clear")) {
        ctl.clearFault(zoneOk ? (uint8_t)(i1 - 1) : irr::NONE);
        statusDue = true;
    } else if (!strcmp(cmd, "auto") && n >= 2) {
        autoMode = !strcmp(s1, "on");
        prefs.putBool("auto", autoMode);
        statusDue = true;
    } else if (!strcmp(cmd, "time") && n >= 2) {
        if (n >= 3) wallClock.tzMin = (int16_t)atoi(s2);
        wallClock.set(strtoul(s1, nullptr, 10));
    } else if (!strcmp(cmd, "prog") && i1 >= 1 && i1 <= sched::MAX_PROGRAMS && n >= 3) {
        sched::Program& p = schedule.programs[i1 - 1];
        unsigned hh = 0, mm = 0, zone = (unsigned)atoi(s2);
        char hhmm[8] = "", days[8] = "";
        float litres = 0;
        int skipPct = 0;
        if (!strcmp(s2, "off")) {
            p.enabled = 0;
        } else if (sscanf(line, "%*s %*s %*s %7s %f %7s %d", hhmm, &litres, days, &skipPct) >= 2 &&
                   sscanf(hhmm, "%u:%u", &hh, &mm) == 2 && hh < 24 && mm < 60 && zone >= 1 && zone <= VALVE_COUNT &&
                   litres > 0) {
            p = sched::Program();
            p.enabled = 1;
            p.zone = (uint8_t)(zone - 1);
            p.startMin = (uint16_t)(hh * 60 + mm);
            p.ml = (uint32_t)(litres * 1000);
            if (days[0]) p.days = (uint8_t)(strtoul(days, nullptr, 16) & 0x7F);
            p.skipAbovePct = (uint8_t)constrain(skipPct, 0, 100);
        } else {
            return help();
        }
        saveSchedule();
        printPrograms();
#ifdef SIM
    } else if (!strcmp(cmd, "sim") && zoneOk && n >= 3) {
        simRate[i1 - 1] = (uint32_t)f2;
#else
    } else if (!strcmp(cmd, "cal") && n >= 2) {
        bool dry = !strcmp(s1, "dry");
        for (uint8_t ch = 0; ch < soil.channels(); ch++) {
            float mv;
            if (!soil.readMilliVolts(ch, mv)) continue;
            (dry ? soil.calibration(ch).dryMv : soil.calibration(ch).wetMv) = mv;
            float v[2] = {soil.calibration(ch).dryMv, soil.calibration(ch).wetMv};
            char key[8];
            snprintf(key, sizeof(key), "cal%u", ch);
            prefs.putBytes(key, v, sizeof(v));
            Serial.printf("  soil %u %s = %.0f mV\n", ch + 1, dry ? "dry" : "wet", mv);
        }
    } else if (!strcmp(cmd, "k") && i1 >= 1 && i1 <= FLOW_COUNT && f2 > 0) {
        char key[8];
        snprintf(key, sizeof(key), "k%u", i1 - 1);
        flow[i1 - 1].setPulsesPerLitre(f2);
        prefs.putFloat(key, f2);
#endif
    } else {
        help();
    }
}

// ----------------------------------------------------------------- main loop
static const char* eventName(irr::EventCode c) {
    static const char* n[] = {"?", "run started", "run finished", "RUN FAULT", "LEAK", "leak cleared"};
    return n[(uint8_t)c <= 5 ? (uint8_t)c : 0];
}

static void applyValves() {
    uint8_t want = ctl.valveMask();
    for (uint8_t i = 0; i < VALVE_COUNT; i++) {
        bool on = want & (1u << i);
        // ValveBank's own time limit is a second, independent cap on top of the controller's.
        if (on && !valves.isOpen(i)) valves.open(i, (ZONE_MAX_RUN_S + 60) * 1000UL);
        if (!on && valves.isOpen(i)) valves.close(i);
    }
    valves.tick();
}

void setup() {
    valves.begin();  // outputs off before anything else
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n=== smart-veggies node %s%s ===\n", FW_VERSION,
#ifdef SIM
                  " [SIMULATED SENSORS]"
#else
                  ""
#endif
    );
#ifdef LINK_KEY_IS_DEFAULT
    Serial.println("WARNING: default LINK_KEY in use. Create include/secrets.h before deploying.");
#endif
    prefs.begin("node", false);
    autoMode = prefs.getBool("auto", true);
    cmdCounter = prefs.getUInt("counter", 0);
    prefs.getBytes("progs", schedule.programs, sizeof(schedule.programs));
    if (ctl.config().masterValve == irr::NONE) valves.setExclusive(true);

#ifdef SIM
    simBegin();
#else
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    for (uint8_t i = 0; i < FLOW_COUNT; i++) flow[i].begin(flowPins[i], FLOW_PULSES_PER_LITRE);
    soil.begin(adsAddresses, sizeof(adsAddresses), adcPins, sizeof(adcPins));
    loadCalibration();
    probes.begin();
    probes.setWaitForConversion(false);
    probes.requestTemperatures();
    dht.setup(PIN_DHT, DHTesp::DHT11);
    Serial.printf("soil channels: %u (%s), DS18B20 probes: %u\n", soil.channels(), soil.usingAds() ? "ADS1115" : "ESP32 ADC",
                  probes.getDeviceCount());
#endif

    radioOk = radio.begin();
    Serial.println(radioOk ? "E32 radio found" : "E32 radio NOT found: running without link");
    lora.setRetry(LINK_RETRY_MS, LINK_ATTEMPTS);

    esp_task_wdt_init(15, true);  // frozen firmware -> reset -> valves.begin() closes everything
    esp_task_wdt_add(nullptr);
    if (radioOk) sendEvent(proto::EV_BOOT, proto::NO_ZONE, (uint8_t)esp_reset_reason(), 0, 0);
    help();
}

void loop() {
    esp_task_wdt_reset();
    static uint32_t lastSensors = 0, lastTelemetry = 0, lastStatus = 0;
    uint32_t now = millis();

#ifdef SIM
    simTick();
#else
    for (auto& f : flow) f.update();
#endif

    // irrigation core
    irr::Inputs in;
    in.nowMs = now;
    for (uint8_t s = 0; s < FLOW_COUNT && s < irr::MAX_FLOW; s++) in.flowMl[s] = flowMl(s);
    ctl.tick(in);
    applyValves();
    irr::Event e;
    while (ctl.popEvent(e)) {
        Serial.printf("[%u] %s: zone/sensor %u detail %u  %.2f L  %u s\n", uptimeS(), eventName(e.code), e.zone + 1, e.detail,
                      e.a / 1000.0f, e.b);
        if (radioOk) sendEvent((uint8_t)e.code, e.zone, e.detail, e.a, e.b);
    }

    runSchedules();
    pollLink();

    if (now - lastSensors >= 10000 || lastSensors == 0) {
        lastSensors = now ? now : 1;
        readSensors();
    }
    if (radioOk && !lora.pending()) {  // events first; periodic data when the queue is empty
        if (statusDue || now - lastStatus >= STATUS_PERIOD_S * 1000UL) {
            statusDue = false;
            lastStatus = now;
            sendStatus();
        } else if (now - lastTelemetry >= TELEMETRY_PERIOD_S * 1000UL) {
            lastTelemetry = now;
            sendTelemetry();
        }
    }

    if (const char* line = console.poll()) handleLine(line);
    delay(5);
}

// Gateway: bridges the LoRa link to MQTT (JSON) over WiFi. Deliberately dumb:
// binary frames in -> JSON out, JSON commands in -> signed frames out.
//
//   cp include/secrets.example.h include/secrets.h   (fill in), then
//   pio run -e gateway -t upload -t monitor
//
// Topics (node "node1"):
//   garden/node1/telemetry | status (retained) | event      node -> server
//   garden/node1/cmd                                        server -> node, e.g.
//       {"cmd":"water","zone":1,"litres":4,"max_s":900,"ttl_s":600,"id":"abc"}
//       {"cmd":"run","zone":2,"seconds":300}   {"cmd":"stop"}   {"cmd":"clear","zone":1}
//       {"cmd":"auto","on":true}               {"cmd":"rain_skip","hours":24}
//   garden/node1/cmd/result                                 outcome of each command
//   garden/gateway/availability (online/offline) | garden/gateway/status
// Zones are numbered from 1 in JSON.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "e32.h"
#include "link.h"
#include "messages.h"

#ifndef WIFI_SSID
#error "Create include/secrets.h from include/secrets.example.h"
#endif

#define NODE "node1"
#define T_BASE "garden/" NODE
#define T_AVAIL "garden/gateway/availability"

static const uint8_t linkKey[] = LINK_KEY;
static const size_t linkKeyLen = sizeof(linkKey) - 1;

static E32 radio(Serial2, PIN_LORA_RX, PIN_LORA_TX, PIN_LORA_MODE, PIN_LORA_AUX);
static LoraLink lora(radio, proto::ADDR_GATEWAY);
static WiFiClient wifi;
static PubSubClient mqtt(wifi);
static Preferences prefs;

static uint32_t counter = 0;       // last used command counter (persisted)
static uint16_t lastEventId = 0;
static bool haveEvent = false;
static bool nodeClockValid = false;
static uint32_t lastTimeSyncMs = 0;
static bool timeSynced = false;

// Commands in flight: remember the caller's id so the result can be matched.
struct PendingCmd {
    bool used = false;
    uint8_t seq = 0;
    bool resynced = false;
    proto::Cmd cmd;
    char id[24] = "";
};
static PendingCmd pendingCmds[8];

static bool clockOk() { return time(nullptr) > 1700000000; }

static void publish(const char* topic, JsonDocument& doc, bool retain = false) {
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic, (const uint8_t*)buf, n, retain);
}

// ---------------------------------------------------------------- LoRa -> MQTT
static const char* stateName(uint8_t s) {
    static const char* n[] = {"idle", "queued", "priming", "dosing", "settling"};
    return s < 5 ? n[s] : "unknown";
}
static const char* eventName(uint8_t c) {
    switch (c) {
        case 1: return "run_started";
        case 2: return "run_finished";
        case 3: return "run_fault";
        case 4: return "leak";
        case 5: return "leak_cleared";
        case proto::EV_BOOT: return "boot";
    }
    return "unknown";
}
static const char* endReason(uint8_t r) {
    static const char* n[] = {"target_reached", "time_limit", "volume_limit", "stopped", "dropped"};
    return r < 5 ? n[r] : "unknown";
}
static const char* faultName(uint8_t f) { return f & 2 ? "burst" : f & 1 ? "no_flow" : "none"; }
static const char* rejectName(uint8_t r) {
    static const char* n[] = {"ok", "bad_zone", "disabled", "locked", "queue_full", "invalid", "daily_limit", "locked_out", "leak"};
    return r < 9 ? n[r] : "unknown";
}
static const char* ackName(uint8_t r) {
    static const char* n[] = {"ok", "bad_auth", "replay", "expired", "rejected", "invalid"};
    return r < 6 ? n[r] : "unknown";
}

static void onTelemetry(const proto::Telemetry& t) {
    JsonDocument d;
    d["t"] = t.epoch ? t.epoch : (uint32_t)time(nullptr);
    JsonArray soil = d["soil"].to<JsonArray>();
    for (uint8_t v : t.soilHalfPct) v == proto::NO_DATA_U8 ? soil.add(nullptr) : soil.add(v / 2.0f);
    JsonArray st = d["soil_temp"].to<JsonArray>();
    for (int16_t v : t.soilTempC100) v == proto::NO_DATA_I16 ? st.add(nullptr) : st.add(v / 100.0f);
    if (t.airTempC100 != proto::NO_DATA_I16) d["air_temp"] = t.airTempC100 / 100.0f;
    if (t.airRhHalfPct != proto::NO_DATA_U8) d["air_rh"] = t.airRhHalfPct / 2.0f;
    if (t.supplyMv) d["supply_v"] = t.supplyMv / 1000.0f;
    JsonArray today = d["today_l"].to<JsonArray>();
    for (uint16_t v : t.todayDl) today.add(v / 10.0f);
    publish(T_BASE "/telemetry", d);
}

static void onStatus(const proto::Status& s) {
    nodeClockValid = s.flags & proto::ST_CLOCK_VALID;
    if (s.cmdCounter > counter) counter = s.cmdCounter;  // e.g. after the gateway was replaced
    JsonDocument d;
    d["t"] = s.epoch ? s.epoch : (uint32_t)time(nullptr);
    d["uptime"] = s.uptimeS;
    d["clock_valid"] = (bool)(s.flags & proto::ST_CLOCK_VALID);
    d["auto"] = (bool)(s.flags & proto::ST_AUTO);
    d["leak"] = (bool)(s.flags & proto::ST_LEAK);
    d["rain_skip"] = (bool)(s.flags & proto::ST_RAIN_SKIP);
    d["sim"] = (bool)(s.flags & proto::ST_SIM);
    if (s.activeZone == proto::NO_ZONE) d["active_zone"] = nullptr; else d["active_zone"] = s.activeZone + 1;
    JsonArray zs = d["zone_state"].to<JsonArray>(), zf = d["zone_fault"].to<JsonArray>(), zv = d["valve"].to<JsonArray>();
    for (uint8_t z = 0; z < proto::ZONES; z++) {
        zs.add(stateName(s.zoneState[z]));
        zf.add(faultName(s.zoneFault[z]));
        zv.add((bool)(s.valveMask & (1u << z)));
    }
    d["run_l"] = s.runMl / 1000.0f;
    d["cmd_counter"] = s.cmdCounter;
    d["reset_reason"] = s.resetReason;
    publish(T_BASE "/status", d, true);
}

static void onEvent(const proto::Frame& f, const proto::Event& e) {
    lora.sendAck(f, proto::ACK_OK);
    if (haveEvent && e.id == lastEventId) return;  // retry of one we already published
    haveEvent = true;
    lastEventId = e.id;
    JsonDocument d;
    d["t"] = e.epoch ? e.epoch : (uint32_t)time(nullptr);
    d["id"] = e.id;
    d["event"] = eventName(e.code);
    if (e.code == 4 || e.code == 5) {
        d["flow_sensor"] = e.zone + 1;
        d["litres"] = e.a / 1000.0f;
    } else if (e.code == proto::EV_BOOT) {
        d["reset_reason"] = e.detail;
        nodeClockValid = false;  // it just rebooted and has no RTC yet
    } else {
        d["zone"] = e.zone + 1;
        if (e.code == 1) {
            d["target_l"] = e.a / 1000.0f;
            d["limit_s"] = e.b;
        } else {
            d["litres"] = e.a / 1000.0f;
            d["seconds"] = e.b;
            d["detail"] = e.code == 3 ? faultName(e.detail) : endReason(e.detail);
        }
    }
    publish(T_BASE "/event", d);
}

// ---------------------------------------------------------------- MQTT -> LoRa
static void cmdResult(const char* id, bool ok, const char* result, const char* detail = nullptr) {
    JsonDocument d;
    if (id && id[0]) d["id"] = id;
    d["ok"] = ok;
    d["result"] = result;
    if (detail) d["detail"] = detail;
    publish(T_BASE "/cmd/result", d);
}

static bool queueCmd(PendingCmd& p) {
    p.cmd.counter = ++counter;
    prefs.putUInt("counter", counter);
    proto::Frame f = lora.make(proto::ADDR_NODE_1);
    proto::encode(p.cmd, f);
    proto::sign(f, linkKey, linkKeyLen);
    p.seq = f.seq;
    p.used = lora.sendReliable(f);
    return p.used;
}

static void onMqtt(char* topic, uint8_t* payload, unsigned int len) {
    JsonDocument d;
    if (deserializeJson(d, payload, len)) return cmdResult("", false, "bad_json");
    const char* id = d["id"] | "";
    const char* name = d["cmd"] | "";
    int zone = d["zone"] | 0;
    proto::Cmd c;
    c.zone = zone >= 1 && zone <= proto::ZONES ? (uint8_t)(zone - 1) : proto::NO_ZONE;
    bool needZone = false;
    if (!strcmp(name, "water")) {
        c.cmd = proto::CMD_WATER_VOLUME;
        c.arg = (uint16_t)constrain((float)(d["litres"] | 0.0f) * 10, 0.0f, 65535.0f);
        c.arg2 = d["max_s"] | 0;
        needZone = true;
        if (!c.arg) return cmdResult(id, false, "invalid", "litres");
    } else if (!strcmp(name, "run")) {
        c.cmd = proto::CMD_WATER_TIME;
        c.arg = d["seconds"] | 0;
        needZone = true;
        if (!c.arg) return cmdResult(id, false, "invalid", "seconds");
    } else if (!strcmp(name, "stop")) {
        c.cmd = proto::CMD_STOP_ALL;
    } else if (!strcmp(name, "clear")) {
        c.cmd = proto::CMD_CLEAR_FAULT;
    } else if (!strcmp(name, "auto")) {
        c.cmd = proto::CMD_SET_AUTO;
        c.arg = (d["on"] | true) ? 1 : 0;
    } else if (!strcmp(name, "rain_skip")) {
        c.cmd = proto::CMD_RAIN_SKIP;
        c.arg = d["hours"] | 0;
    } else {
        return cmdResult(id, false, "invalid", "cmd");
    }
    if (needZone && c.zone == proto::NO_ZONE) return cmdResult(id, false, "invalid", "zone");
    // Watering intents expire: a command stuck behind a dead link must not fire hours later.
    uint32_t ttl = d["ttl_s"] | 600;
    if (clockOk() && c.cmd != proto::CMD_STOP_ALL) c.expiry = (uint32_t)time(nullptr) + ttl;

    for (auto& p : pendingCmds) {
        if (p.used) continue;
        p = PendingCmd();
        p.cmd = c;
        strlcpy(p.id, id, sizeof(p.id));
        if (!queueCmd(p)) cmdResult(id, false, "busy");
        return;
    }
    cmdResult(id, false, "busy");
}

static void sendTime() {
    proto::TimeSync t;
    t.counter = ++counter;
    prefs.putUInt("counter", counter);
    t.epoch = (uint32_t)time(nullptr);
    t.tzMinutes = TZ_OFFSET_MINUTES;
    proto::Frame f = lora.make(proto::ADDR_NODE_1);
    proto::encode(t, f);
    proto::sign(f, linkKey, linkKeyLen);
    lora.sendReliable(f);
    lastTimeSyncMs = millis();
}

static void onLinkResult(const LoraLink::Result& r) {
    if (r.frame.type == proto::MSG_TIME) {
        if (r.acked && r.ack.result == proto::ACK_REPLAY) {
            counter = r.ack.counter;  // node is ahead of us: adopt and try again
            sendTime();
        } else if (r.acked && r.ack.result == proto::ACK_OK) {
            timeSynced = nodeClockValid = true;
        }
        return;
    }
    for (auto& p : pendingCmds) {
        if (!p.used || p.seq != r.frame.seq) continue;
        p.used = false;
        if (!r.acked) return cmdResult(p.id, false, "no_answer");
        if (r.ack.result == proto::ACK_REPLAY && !p.resynced) {
            counter = r.ack.counter;
            p.resynced = true;
            if (queueCmd(p)) return;
        }
        bool ok = r.ack.result == proto::ACK_OK;
        return cmdResult(p.id, ok, ackName(r.ack.result),
                         r.ack.result == proto::ACK_REJECTED ? rejectName(r.ack.detail) : nullptr);
    }
}

// --------------------------------------------------------- Home Assistant
static void discover(const char* component, const char* key, const char* name, const char* topic, const char* tmpl,
                     const char* unit = nullptr, const char* devClass = nullptr, const char* cmdPayload = nullptr) {
    JsonDocument d;
    char uid[48], t[96];
    snprintf(uid, sizeof(uid), "smartveggies_" NODE "_%s", key);
    d["name"] = name;
    d["unique_id"] = uid;
    d["availability_topic"] = T_AVAIL;
    if (cmdPayload) {
        d["command_topic"] = topic;
        d["payload_press"] = cmdPayload;
    } else {
        d["state_topic"] = topic;
        d["value_template"] = tmpl;
    }
    if (unit) d["unit_of_measurement"] = unit;
    if (devClass) d["device_class"] = devClass;
    if (unit && strcmp(component, "sensor") == 0) d["state_class"] = "measurement";
    JsonObject dev = d["device"].to<JsonObject>();
    dev["identifiers"][0] = "smartveggies_" NODE;
    dev["name"] = "Garden " NODE;
    dev["manufacturer"] = "smart-veggies";
    dev["sw_version"] = FW_VERSION;
    snprintf(t, sizeof(t), "homeassistant/%s/smartveggies_" NODE "/%s/config", component, key);
    publish(t, d, true);
}

static void publishDiscovery() {
    char key[24], name[40], tmpl[80], payload[64];
    for (int i = 0; i < proto::SOIL_CHANNELS; i++) {
        snprintf(key, sizeof(key), "soil_%d", i + 1);
        snprintf(name, sizeof(name), "Soil moisture %d", i + 1);
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.soil[%d] }}", i);
        discover("sensor", key, name, T_BASE "/telemetry", tmpl, "%", "moisture");
    }
    for (int i = 0; i < proto::SOIL_TEMPS; i++) {
        snprintf(key, sizeof(key), "soil_temp_%d", i + 1);
        snprintf(name, sizeof(name), "Soil temperature %d", i + 1);
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.soil_temp[%d] }}", i);
        discover("sensor", key, name, T_BASE "/telemetry", tmpl, "°C", "temperature");
    }
    discover("sensor", "air_temp", "Air temperature", T_BASE "/telemetry", "{{ value_json.air_temp }}", "°C", "temperature");
    discover("sensor", "air_rh", "Air humidity", T_BASE "/telemetry", "{{ value_json.air_rh }}", "%", "humidity");
    for (int z = 0; z < proto::ZONES; z++) {
        snprintf(key, sizeof(key), "zone_%d_today", z + 1);
        snprintf(name, sizeof(name), "Zone %d water today", z + 1);
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.today_l[%d] }}", z);
        discover("sensor", key, name, T_BASE "/telemetry", tmpl, "L", "water");
        snprintf(key, sizeof(key), "zone_%d_state", z + 1);
        snprintf(name, sizeof(name), "Zone %d state", z + 1);
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.zone_state[%d] }}", z);
        discover("sensor", key, name, T_BASE "/status", tmpl);
        snprintf(key, sizeof(key), "zone_%d_fault", z + 1);
        snprintf(name, sizeof(name), "Zone %d fault", z + 1);
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.zone_fault[%d] }}", z);
        discover("sensor", key, name, T_BASE "/status", tmpl);
        snprintf(key, sizeof(key), "zone_%d_water_2l", z + 1);
        snprintf(name, sizeof(name), "Zone %d: water 2 L", z + 1);
        snprintf(payload, sizeof(payload), "{\"cmd\":\"water\",\"zone\":%d,\"litres\":2}", z + 1);
        discover("button", key, name, T_BASE "/cmd", nullptr, nullptr, nullptr, payload);
        mqtt.loop();
    }
    discover("binary_sensor", "leak", "Leak alarm", T_BASE "/status", "{{ 'ON' if value_json.leak else 'OFF' }}", nullptr,
             "problem");
    discover("binary_sensor", "auto", "Schedules enabled", T_BASE "/status", "{{ 'ON' if value_json.auto else 'OFF' }}");
    discover("button", "stop_all", "Stop all watering", T_BASE "/cmd", nullptr, nullptr, nullptr, "{\"cmd\":\"stop\"}");
}

// ------------------------------------------------------------------ plumbing
static void ensureConnected() {
    static uint32_t lastTry = 0;
    if (WiFi.status() != WL_CONNECTED || mqtt.connected() || millis() - lastTry < 5000) return;
    lastTry = millis();
    if (mqtt.connect("smartveggies-gateway", MQTT_USER, MQTT_PASS, T_AVAIL, 1, true, "offline")) {
        mqtt.publish(T_AVAIL, "online", true);
        mqtt.subscribe(T_BASE "/cmd", 1);
        publishDiscovery();
        Serial.println("MQTT connected");
    } else {
        Serial.printf("MQTT connect failed, state %d\n", mqtt.state());
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n=== smart-veggies gateway %s ===\n", FW_VERSION);
    prefs.begin("gateway", false);
    counter = prefs.getUInt("counter", 0);
    bool radioOk = radio.begin();
    Serial.println(radioOk ? "E32 radio found" : "E32 radio NOT found");
    lora.setRetry(LINK_RETRY_MS, LINK_ATTEMPTS);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1100);
    mqtt.setCallback(onMqtt);
}

void loop() {
    ensureConnected();
    mqtt.loop();

    proto::Frame f;
    while (lora.poll(f)) {
        if (f.src != proto::ADDR_NODE_1) continue;
        proto::Telemetry t;
        proto::Status s;
        proto::Event e;
        if (proto::decode(f, t)) onTelemetry(t);
        else if (proto::decode(f, s)) onStatus(s);
        else if (proto::decode(f, e)) onEvent(f, e);
    }
    LoraLink::Result r;
    while (lora.popResult(r)) onLinkResult(r);

    // Keep the node's clock set: when it reports no clock, and every 6 h.
    uint32_t now = millis();
    bool heardNode = lora.lastRxMs() && now - lora.lastRxMs() < 30000;
    if (clockOk() && heardNode && !lora.pending() &&
        ((!nodeClockValid && now - lastTimeSyncMs > 30000) || !timeSynced || now - lastTimeSyncMs > 6UL * 3600 * 1000)) {
        if (now - lastTimeSyncMs > 30000 || lastTimeSyncMs == 0) sendTime();
    }

    static uint32_t lastStatus = 0;
    if (mqtt.connected() && now - lastStatus > 60000) {
        lastStatus = now;
        JsonDocument d;
        d["uptime"] = now / 1000;
        d["wifi_rssi"] = WiFi.RSSI();
        d["lora_tx"] = lora.txCount;
        d["lora_rx"] = lora.rxCount;
        d["lora_retries"] = lora.retries;
        d["lora_failed"] = lora.failed;
        d["node_last_seen_s"] = lora.lastRxMs() ? (now - lora.lastRxMs()) / 1000 : -1;
        publish("garden/gateway/status", d, true);
    }
    delay(5);
}

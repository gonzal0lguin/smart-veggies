#include "messages.h"

#include <string.h>

#include "sha256.h"

namespace proto {

namespace {

struct Writer {
    Frame& f;
    Writer(Frame& frame, uint8_t type) : f(frame) {
        f.type = type;
        f.len = 0;
    }
    void u8(uint8_t v) { f.payload[f.len++] = v; }
    void u16(uint16_t v) {
        u8((uint8_t)v);
        u8((uint8_t)(v >> 8));
    }
    void u32(uint32_t v) {
        u16((uint16_t)v);
        u16((uint16_t)(v >> 16));
    }
};

struct Reader {
    const Frame& f;
    uint8_t pos = 0;
    explicit Reader(const Frame& frame) : f(frame) {}
    uint8_t u8() { return f.payload[pos++]; }
    uint16_t u16() {
        uint16_t lo = u8();
        return (uint16_t)(lo | (uint16_t)u8() << 8);
    }
    uint32_t u32() {
        uint32_t lo = u16();
        return lo | (uint32_t)u16() << 16;
    }
};

constexpr uint8_t LEN_TELEMETRY = 4 + SOIL_CHANNELS + 2 * SOIL_TEMPS + 2 + 1 + 2 + 2 * ZONES;
constexpr uint8_t LEN_STATUS = 4 + 4 + 1 + 1 + ZONES + ZONES + 1 + 4 + 4 + 1;
constexpr uint8_t LEN_EVENT = 4 + 2 + 1 + 1 + 1 + 4 + 4;
constexpr uint8_t LEN_CMD = 4 + 1 + 1 + 2 + 2 + 4;
constexpr uint8_t LEN_TIME = 4 + 4 + 2;
constexpr uint8_t LEN_ACK = 1 + 1 + 1 + 1 + 4;

static_assert(LEN_TELEMETRY <= MAX_PAYLOAD, "telemetry too long");
static_assert(LEN_STATUS <= MAX_PAYLOAD, "status too long");
static_assert(LEN_CMD + MAC_LEN <= MAX_PAYLOAD, "cmd too long");

bool is(const Frame& f, uint8_t type, uint8_t len) { return f.type == type && f.len == len; }

void mac(const Frame& f, uint8_t len, const uint8_t* key, size_t keyLen, uint8_t out[MAC_LEN]) {
    uint8_t msg[4 + MAX_PAYLOAD] = {f.dst, f.src, f.type, f.seq};
    memcpy(msg + 4, f.payload, len);
    uint8_t full[32];
    hmacSha256(key, keyLen, msg, 4 + (size_t)len, full);
    memcpy(out, full, MAC_LEN);
}

}  // namespace

Telemetry::Telemetry() {
    memset(soilHalfPct, NO_DATA_U8, sizeof(soilHalfPct));
    for (auto& t : soilTempC100) t = NO_DATA_I16;
}

void encode(const Telemetry& m, Frame& f) {
    Writer w(f, MSG_TELEMETRY);
    w.u32(m.epoch);
    for (uint8_t v : m.soilHalfPct) w.u8(v);
    for (int16_t v : m.soilTempC100) w.u16((uint16_t)v);
    w.u16((uint16_t)m.airTempC100);
    w.u8(m.airRhHalfPct);
    w.u16(m.supplyMv);
    for (uint16_t v : m.todayDl) w.u16(v);
}

bool decode(const Frame& f, Telemetry& m) {
    if (!is(f, MSG_TELEMETRY, LEN_TELEMETRY)) return false;
    Reader r(f);
    m.epoch = r.u32();
    for (uint8_t& v : m.soilHalfPct) v = r.u8();
    for (int16_t& v : m.soilTempC100) v = (int16_t)r.u16();
    m.airTempC100 = (int16_t)r.u16();
    m.airRhHalfPct = r.u8();
    m.supplyMv = r.u16();
    for (uint16_t& v : m.todayDl) v = r.u16();
    return true;
}

void encode(const Status& m, Frame& f) {
    Writer w(f, MSG_STATUS);
    w.u32(m.epoch);
    w.u32(m.uptimeS);
    w.u8(m.flags);
    w.u8(m.activeZone);
    for (uint8_t v : m.zoneState) w.u8(v);
    for (uint8_t v : m.zoneFault) w.u8(v);
    w.u8(m.valveMask);
    w.u32(m.runMl);
    w.u32(m.cmdCounter);
    w.u8(m.resetReason);
}

bool decode(const Frame& f, Status& m) {
    if (!is(f, MSG_STATUS, LEN_STATUS)) return false;
    Reader r(f);
    m.epoch = r.u32();
    m.uptimeS = r.u32();
    m.flags = r.u8();
    m.activeZone = r.u8();
    for (uint8_t& v : m.zoneState) v = r.u8();
    for (uint8_t& v : m.zoneFault) v = r.u8();
    m.valveMask = r.u8();
    m.runMl = r.u32();
    m.cmdCounter = r.u32();
    m.resetReason = r.u8();
    return true;
}

void encode(const Event& m, Frame& f) {
    Writer w(f, MSG_EVENT);
    w.u32(m.epoch);
    w.u16(m.id);
    w.u8(m.code);
    w.u8(m.zone);
    w.u8(m.detail);
    w.u32(m.a);
    w.u32(m.b);
}

bool decode(const Frame& f, Event& m) {
    if (!is(f, MSG_EVENT, LEN_EVENT)) return false;
    Reader r(f);
    m.epoch = r.u32();
    m.id = r.u16();
    m.code = r.u8();
    m.zone = r.u8();
    m.detail = r.u8();
    m.a = r.u32();
    m.b = r.u32();
    return true;
}

void encode(const Cmd& m, Frame& f) {
    Writer w(f, MSG_CMD);
    w.u32(m.counter);
    w.u8(m.cmd);
    w.u8(m.zone);
    w.u16(m.arg);
    w.u16(m.arg2);
    w.u32(m.expiry);
}

bool decode(const Frame& f, Cmd& m) {
    if (!is(f, MSG_CMD, LEN_CMD)) return false;
    Reader r(f);
    m.counter = r.u32();
    m.cmd = r.u8();
    m.zone = r.u8();
    m.arg = r.u16();
    m.arg2 = r.u16();
    m.expiry = r.u32();
    return true;
}

void encode(const TimeSync& m, Frame& f) {
    Writer w(f, MSG_TIME);
    w.u32(m.counter);
    w.u32(m.epoch);
    w.u16((uint16_t)m.tzMinutes);
}

bool decode(const Frame& f, TimeSync& m) {
    if (!is(f, MSG_TIME, LEN_TIME)) return false;
    Reader r(f);
    m.counter = r.u32();
    m.epoch = r.u32();
    m.tzMinutes = (int16_t)r.u16();
    return true;
}

void encode(const Ack& m, Frame& f) {
    Writer w(f, MSG_ACK);
    w.u8(m.seq);
    w.u8(m.type);
    w.u8(m.result);
    w.u8(m.detail);
    w.u32(m.counter);
}

bool decode(const Frame& f, Ack& m) {
    if (!is(f, MSG_ACK, LEN_ACK)) return false;
    Reader r(f);
    m.seq = r.u8();
    m.type = r.u8();
    m.result = r.u8();
    m.detail = r.u8();
    m.counter = r.u32();
    return true;
}

bool sign(Frame& f, const uint8_t* key, size_t keyLen) {
    if (f.len + MAC_LEN > MAX_PAYLOAD) return false;
    mac(f, f.len, key, keyLen, f.payload + f.len);
    f.len += MAC_LEN;
    return true;
}

bool verify(Frame& f, const uint8_t* key, size_t keyLen) {
    if (f.len < MAC_LEN || f.len > MAX_PAYLOAD) return false;
    uint8_t expect[MAC_LEN];
    mac(f, f.len - MAC_LEN, key, keyLen, expect);
    uint8_t diff = 0;
    for (uint8_t i = 0; i < MAC_LEN; i++) diff |= expect[i] ^ f.payload[f.len - MAC_LEN + i];
    if (diff) return false;
    f.len -= MAC_LEN;
    return true;
}

}  // namespace proto

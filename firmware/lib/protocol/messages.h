#pragma once
// Application messages carried in proto::Frame payloads. Fixed little-endian
// layouts, every one fits a single frame. No Arduino dependencies.
//
// Authenticated messages (gateway -> node) start with a 32-bit rolling counter
// and end with a 4-byte truncated HMAC-SHA-256 over dst, src, type, seq and
// the payload. The node only accepts a counter higher than the last accepted.
#include "frame.h"

namespace proto {

constexpr uint8_t SOIL_CHANNELS = 12;
constexpr uint8_t SOIL_TEMPS = 2;
constexpr uint8_t ZONES = 6;
constexpr uint8_t MAC_LEN = 4;

constexpr uint8_t NO_DATA_U8 = 0xFF;
constexpr int16_t NO_DATA_I16 = INT16_MIN;
constexpr uint8_t NO_ZONE = 0xFF;

struct Telemetry {
    uint32_t epoch = 0;                   // UTC seconds, 0 = clock not set
    uint8_t soilHalfPct[SOIL_CHANNELS];   // moisture in 0.5 % steps (0..200)
    int16_t soilTempC100[SOIL_TEMPS];     // 0.01 C
    int16_t airTempC100 = NO_DATA_I16;
    uint8_t airRhHalfPct = NO_DATA_U8;
    uint16_t supplyMv = 0;                // 0 = not measured
    uint16_t todayDl[ZONES] = {};         // decilitres delivered today per zone
    Telemetry();
};

enum StatusFlags : uint8_t {
    ST_CLOCK_VALID = 1,
    ST_AUTO = 2,       // schedules enabled
    ST_LEAK = 4,       // flow with every valve closed
    ST_SIM = 8,        // simulated sensors
    ST_RAIN_SKIP = 16,
};

struct Status {
    uint32_t epoch = 0;
    uint32_t uptimeS = 0;
    uint8_t flags = 0;
    uint8_t activeZone = NO_ZONE;
    uint8_t zoneState[ZONES] = {};  // irr::State
    uint8_t zoneFault[ZONES] = {};  // irr::Fault bits (non-zero = locked)
    uint8_t valveMask = 0;
    uint32_t runMl = 0;             // delivered so far in the active run
    uint32_t cmdCounter = 0;        // last accepted command counter
    uint8_t resetReason = 0;
};

struct Event {
    uint32_t epoch = 0;
    uint16_t id = 0;     // increments per event, lets the server spot gaps/duplicates
    uint8_t code = 0;    // irr::EventCode, or EV_BOOT
    uint8_t zone = NO_ZONE;
    uint8_t detail = 0;
    uint32_t a = 0;
    uint32_t b = 0;
};
constexpr uint8_t EV_BOOT = 0x80;

enum CmdCode : uint8_t {
    CMD_WATER_VOLUME = 1,  // arg = decilitres, arg2 = max seconds (0 = zone default)
    CMD_WATER_TIME = 2,    // arg = seconds
    CMD_STOP_ALL = 3,
    CMD_CLEAR_FAULT = 4,   // zone, or NO_ZONE for all
    CMD_SET_AUTO = 5,      // arg = 0/1
    CMD_RAIN_SKIP = 6,     // arg = hours to suspend schedules (0 = cancel)
};

struct Cmd {
    uint32_t counter = 0;
    uint8_t cmd = 0;
    uint8_t zone = NO_ZONE;
    uint16_t arg = 0;
    uint16_t arg2 = 0;
    uint32_t expiry = 0;  // UTC seconds after which the node must ignore it, 0 = none
};

struct TimeSync {
    uint32_t counter = 0;
    uint32_t epoch = 0;     // UTC
    int16_t tzMinutes = 0;  // local = UTC + tzMinutes
};

enum AckResult : uint8_t {
    ACK_OK = 0,
    ACK_BAD_AUTH = 1,
    ACK_REPLAY = 2,   // counter too low; Ack.counter tells the sender where the node is
    ACK_EXPIRED = 3,
    ACK_REJECTED = 4, // valid but refused; Ack.detail = irr::Reject
    ACK_INVALID = 5,
};

struct Ack {
    uint8_t seq = 0;
    uint8_t type = 0;
    uint8_t result = 0;
    uint8_t detail = 0;
    uint32_t counter = 0;
};

// encode(): fills frame.type, frame.len and the payload (dst/src/seq are the
// caller's). decode(): false if the type or length does not match.
void encode(const Telemetry& m, Frame& f);
void encode(const Status& m, Frame& f);
void encode(const Event& m, Frame& f);
void encode(const Cmd& m, Frame& f);
void encode(const TimeSync& m, Frame& f);
void encode(const Ack& m, Frame& f);
bool decode(const Frame& f, Telemetry& m);
bool decode(const Frame& f, Status& m);
bool decode(const Frame& f, Event& m);
bool decode(const Frame& f, Cmd& m);
bool decode(const Frame& f, TimeSync& m);
bool decode(const Frame& f, Ack& m);

// Appends the MAC to the payload (call after encode() and after setting
// dst/src/seq). False if there is no room.
bool sign(Frame& f, const uint8_t* key, size_t keyLen);
// Checks and strips the MAC. On failure the frame is left untouched.
bool verify(Frame& f, const uint8_t* key, size_t keyLen);

}  // namespace proto

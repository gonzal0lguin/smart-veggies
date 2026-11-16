#pragma once
// Link-layer frame used on the LoRa (E32 transparent UART) link.
// No Arduino dependencies: unit-tested on the host (pio test -e native).
//
//   SYNC  LEN  DST  SRC  TYPE  SEQ  PAYLOAD[LEN]  CRC16(hi, lo)
//   0xA5   1    1    1    1     1    0..48          over LEN..PAYLOAD
//
// The whole frame is at most 56 bytes so it always fits in a single E32
// air packet (58 bytes).
#include <stddef.h>
#include <stdint.h>

namespace proto {

constexpr uint8_t SYNC = 0xA5;
constexpr size_t MAX_PAYLOAD = 48;
constexpr size_t OVERHEAD = 8;
constexpr size_t MAX_FRAME = MAX_PAYLOAD + OVERHEAD;

constexpr uint8_t ADDR_GATEWAY = 0x00;
constexpr uint8_t ADDR_NODE_1 = 0x01;
constexpr uint8_t ADDR_BROADCAST = 0xFF;

enum MsgType : uint8_t {
    MSG_PING = 0x01,
    MSG_PONG = 0x02,
    MSG_ACK = 0x03,
    // node -> gateway
    MSG_TELEMETRY = 0x10,
    MSG_STATUS = 0x11,
    MSG_EVENT = 0x12,  // acked + retried
    // gateway -> node, authenticated (counter + truncated HMAC), acked
    MSG_CMD = 0x20,
    MSG_TIME = 0x21,
    // reserved: 0x13 BACKFILL, 0x22 CONFIG
};

struct Frame {
    uint8_t dst = 0;
    uint8_t src = 0;
    uint8_t type = 0;
    uint8_t seq = 0;
    uint8_t len = 0;
    uint8_t payload[MAX_PAYLOAD] = {};
};

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF).
uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF);

// Serializes a frame. Returns the number of bytes written, 0 if it does not
// fit or frame.len is invalid.
size_t encode(const Frame& frame, uint8_t* out, size_t capacity);

// Byte-stream parser: feed received bytes one at a time.
class Parser {
   public:
    // Returns true when `out` holds a complete, CRC-valid frame.
    bool feed(uint8_t byte, Frame& out);
    // Drop a partially received frame (call after a long gap between bytes).
    void reset();
    bool idle() const { return state_ == State::Sync; }
    uint32_t crcErrors() const { return crcErrors_; }

   private:
    enum class State : uint8_t { Sync, Len, Body };
    State state_ = State::Sync;
    uint8_t buf_[MAX_FRAME] = {};
    size_t pos_ = 0;
    size_t need_ = 0;
    uint32_t crcErrors_ = 0;
};

}  // namespace proto

#include "frame.h"

#include <string.h>

namespace proto {

uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t encode(const Frame& f, uint8_t* out, size_t capacity) {
    if (f.len > MAX_PAYLOAD) return 0;
    size_t total = OVERHEAD + f.len;
    if (capacity < total) return 0;
    out[0] = SYNC;
    out[1] = f.len;
    out[2] = f.dst;
    out[3] = f.src;
    out[4] = f.type;
    out[5] = f.seq;
    memcpy(out + 6, f.payload, f.len);
    uint16_t crc = crc16(out + 1, 5 + f.len);
    out[6 + f.len] = (uint8_t)(crc >> 8);
    out[7 + f.len] = (uint8_t)(crc & 0xFF);
    return total;
}

void Parser::reset() {
    state_ = State::Sync;
    pos_ = 0;
}

bool Parser::feed(uint8_t byte, Frame& out) {
    switch (state_) {
        case State::Sync:
            if (byte == SYNC) state_ = State::Len;
            return false;

        case State::Len:
            if (byte > MAX_PAYLOAD) {
                // Not a valid length: resync (this byte may itself be a SYNC).
                state_ = (byte == SYNC) ? State::Len : State::Sync;
                return false;
            }
            buf_[0] = byte;
            pos_ = 1;
            need_ = 1 + 4 + byte + 2;  // len + header + payload + crc
            state_ = State::Body;
            return false;

        case State::Body: {
            buf_[pos_++] = byte;
            if (pos_ < need_) return false;
            reset();
            uint8_t len = buf_[0];
            uint16_t expected = crc16(buf_, 5 + len);
            uint16_t got = ((uint16_t)buf_[5 + len] << 8) | buf_[6 + len];
            if (expected != got) {
                crcErrors_++;
                return false;
            }
            out.len = len;
            out.dst = buf_[1];
            out.src = buf_[2];
            out.type = buf_[3];
            out.seq = buf_[4];
            memcpy(out.payload, buf_ + 5, len);
            return true;
        }
    }
    return false;
}

}  // namespace proto

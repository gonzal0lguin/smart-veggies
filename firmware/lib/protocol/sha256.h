#pragma once
// Small portable SHA-256 / HMAC-SHA-256 (no Arduino or mbedTLS dependency so
// the same code is unit-tested on the host).
#include <stddef.h>
#include <stdint.h>

namespace proto {

class Sha256 {
   public:
    Sha256() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void finish(uint8_t out[32]);

   private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_;
    size_t fill_;
};

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[32]);

}  // namespace proto

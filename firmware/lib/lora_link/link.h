#pragma once
// Frame transport on top of the E32: receive parsing, address filtering and a
// small ack/retry queue for messages that must arrive (events, commands).
// Used by both the node and the gateway.
#include <Arduino.h>

#include "e32.h"
#include "messages.h"

class LoraLink {
   public:
    struct Result {
        proto::Frame frame;  // what was sent
        bool acked = false;
        proto::Ack ack;      // valid when acked
    };

    LoraLink(E32& radio, uint8_t address) : radio_(radio), addr_(address) {}

    // retryMs must exceed frame air time + ACK air time (tune after the range test).
    void setRetry(uint32_t retryMs, uint8_t attempts) {
        retryMs_ = retryMs;
        attempts_ = attempts;
    }

    // A frame addressed to `dst` with src and a fresh seq filled in.
    proto::Frame make(uint8_t dst);

    bool send(const proto::Frame& f);          // once, no ack
    bool sendReliable(const proto::Frame& f);  // queued, retried until acked; false = queue full
    void sendAck(const proto::Frame& to, uint8_t result, uint8_t detail = 0, uint32_t counter = 0);

    // Call often. True when `in` holds a received frame for this address
    // (ACKs answering our reliable frames are consumed here).
    bool poll(proto::Frame& in);
    bool popResult(Result& r);

    uint8_t pending() const { return qCount_; }
    uint32_t lastRxMs() const { return lastFrameMs_; }
    uint32_t txCount = 0, rxCount = 0, retries = 0, failed = 0;

   private:
    static constexpr uint8_t QUEUE = 8;
    void transmit(const proto::Frame& f);
    void finish(bool acked, const proto::Ack* ack);

    E32& radio_;
    uint8_t addr_;
    uint8_t seq_ = 0;
    proto::Parser parser_;
    uint32_t lastByteMs_ = 0, lastFrameMs_ = 0;
    uint32_t retryMs_ = 4000;
    uint8_t attempts_ = 4;

    proto::Frame queue_[QUEUE];
    uint8_t qHead_ = 0, qCount_ = 0;
    uint8_t tries_ = 0;
    uint32_t sentMs_ = 0;
    uint32_t backoffMs_ = 0;

    Result results_[4];
    uint8_t rHead_ = 0, rCount_ = 0;
};

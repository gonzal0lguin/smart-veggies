#include "link.h"

using namespace proto;

Frame LoraLink::make(uint8_t dst) {
    Frame f;
    f.dst = dst;
    f.src = addr_;
    f.seq = seq_++;
    return f;
}

void LoraLink::transmit(const Frame& f) {
    uint8_t buf[MAX_FRAME];
    size_t n = encode(f, buf, sizeof(buf));
    if (!n) return;
    radio_.waitIdle(1500);
    radio_.send(buf, n);
    txCount++;
}

bool LoraLink::send(const Frame& f) {
    transmit(f);
    return true;
}

bool LoraLink::sendReliable(const Frame& f) {
    if (qCount_ >= QUEUE) return false;
    queue_[(qHead_ + qCount_) % QUEUE] = f;
    qCount_++;
    return true;
}

void LoraLink::sendAck(const Frame& to, uint8_t result, uint8_t detail, uint32_t counter) {
    Ack a;
    a.seq = to.seq;
    a.type = to.type;
    a.result = result;
    a.detail = detail;
    a.counter = counter;
    Frame f = make(to.src);
    encode(a, f);
    transmit(f);
}

void LoraLink::finish(bool acked, const Ack* ack) {
    if (rCount_ == 4) {  // nobody is reading results: drop the oldest
        rHead_ = (rHead_ + 1) % 4;
        rCount_--;
    }
    Result& r = results_[(rHead_ + rCount_) % 4];
    r.frame = queue_[qHead_];
    r.acked = acked;
    if (ack) r.ack = *ack;
    rCount_++;
    qHead_ = (qHead_ + 1) % QUEUE;
    qCount_--;
    tries_ = 0;
    if (!acked) failed++;
}

bool LoraLink::popResult(Result& r) {
    if (!rCount_) return false;
    r = results_[rHead_];
    rHead_ = (rHead_ + 1) % 4;
    rCount_--;
    return true;
}

bool LoraLink::poll(Frame& in) {
    uint32_t now = millis();
    bool got = false;
    while (!got && radio_.available()) {
        lastByteMs_ = now;
        if (!parser_.feed((uint8_t)radio_.read(), in)) continue;
        if (in.dst != addr_ && in.dst != ADDR_BROADCAST) continue;
        rxCount++;
        lastFrameMs_ = now;
        Ack a;
        if (in.type == MSG_ACK && qCount_ && tries_ && decode(in, a) && a.seq == queue_[qHead_].seq &&
            a.type == queue_[qHead_].type && in.src == queue_[qHead_].dst) {
            finish(true, &a);
            continue;
        }
        got = true;
    }
    // A frame cut short by a lost byte must not swallow the next one.
    if (!parser_.idle() && now - lastByteMs_ > 300) parser_.reset();

    // Reliable queue: one frame in flight; never start talking mid-reception.
    if (qCount_ && parser_.idle() && now - lastByteMs_ > 60) {
        if (tries_ == 0 || now - sentMs_ >= retryMs_ + backoffMs_) {
            if (tries_ >= attempts_) {
                finish(false, nullptr);
            } else {
                if (tries_) retries++;
                transmit(queue_[qHead_]);
                tries_++;
                sentMs_ = millis();
                backoffMs_ = random(0, 800);  // de-synchronise both ends' retries
            }
        }
    }
    return got;
}

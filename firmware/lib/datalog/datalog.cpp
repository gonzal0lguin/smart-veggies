#include "datalog.h"

#include <string.h>

#include "frame.h"  // proto::crc16

namespace dlog {

static constexpr size_t MAX_LISTED = 256;

void Log::begin(uint32_t savedCursor) {
    static uint32_t segs[MAX_LISTED];
    size_t n = st_.list(segs, MAX_LISTED);
    if (!n) {
        // Empty (new or reformatted) storage: keep numbering ahead of the old cursor.
        first_ = head_ = cursor_ = (savedCursor + SEG_RECORDS - 1) / SEG_RECORDS * SEG_RECORDS;
        return;
    }
    uint32_t lo = segs[0], hi = segs[0];
    for (size_t i = 1; i < n; i++) {
        if (segs[i] < lo) lo = segs[i];
        if (segs[i] > hi) hi = segs[i];
    }
    // Anything outside the quota window (stray or left-over files) goes.
    uint32_t keepFrom = hi + 1 >= maxSeg_ ? hi + 1 - maxSeg_ : 0;
    for (size_t i = 0; i < n; i++)
        if (segs[i] < keepFrom) st_.remove(segs[i]);
    if (lo < keepFrom) lo = keepFrom;

    first_ = lo * SEG_RECORDS;
    size_t bytes = st_.size(hi);
    uint32_t whole = (uint32_t)(bytes / SLOT);
    // A torn last write (power cut) or an over-long file: continue in a fresh segment.
    head_ = (bytes % SLOT || whole > SEG_RECORDS) ? (hi + 1) * SEG_RECORDS : hi * SEG_RECORDS + whole;
    cursor_ = savedCursor < first_ ? first_ : savedCursor > head_ ? head_ : savedCursor;
}

bool Log::append(uint8_t type, const uint8_t* data, uint8_t len) {
    if (len > MAX_DATA) return false;
    uint32_t seg = head_ / SEG_RECORDS, idx = head_ % SEG_RECORDS;
    if (idx == 0) {  // starting a segment: make room
        st_.remove(seg);  // a left-over file with this number would corrupt positions
        while (seg - first_ / SEG_RECORDS + 1 > maxSeg_) {
            st_.remove(first_ / SEG_RECORDS);
            first_ = (first_ / SEG_RECORDS + 1) * SEG_RECORDS;
        }
        if (cursor_ < first_) {
            dropped += first_ - cursor_;
            cursor_ = first_;
        }
    }
    uint8_t slot[SLOT] = {};
    slot[0] = MAGIC;
    slot[1] = type;
    slot[2] = len;
    if (len) memcpy(slot + 3, data, len);
    uint16_t crc = proto::crc16(slot, SLOT - 2);
    slot[SLOT - 2] = (uint8_t)(crc & 0xFF);
    slot[SLOT - 1] = (uint8_t)(crc >> 8);

    if (st_.append(seg, slot, SLOT)) {
        head_++;
        return true;
    }
    writeErrors++;
    // Nothing written: try this slot again next time. Partly written: the
    // segment's positions are off, so abandon the rest of it.
    if (st_.size(seg) != (size_t)idx * SLOT) head_ = (seg + 1) * SEG_RECORDS;
    return false;
}

bool Log::read(uint32_t seq, Record& r) {
    if (seq < first_ || seq >= head_) return false;
    uint8_t slot[SLOT];
    if (!st_.read(seq / SEG_RECORDS, (size_t)(seq % SEG_RECORDS) * SLOT, slot, SLOT)) return false;
    uint16_t crc = (uint16_t)(slot[SLOT - 2] | (slot[SLOT - 1] << 8));
    if (slot[0] != MAGIC || slot[2] > MAX_DATA || crc != proto::crc16(slot, SLOT - 2)) return false;
    r.seq = seq;
    r.type = slot[1];
    r.len = slot[2];
    memcpy(r.data, slot + 3, MAX_DATA);
    return true;
}

bool Log::peek(Record& r) {
    while (cursor_ < head_) {
        if (read(cursor_, r)) return true;
        corrupt++;
        cursor_++;
    }
    return false;
}

void Log::ack(uint32_t seq) {
    if (seq >= cursor_ && seq < head_) cursor_ = seq + 1;
}

void Log::clear() {
    for (uint32_t s = first_ / SEG_RECORDS; s <= head_ / SEG_RECORDS; s++) st_.remove(s);
    head_ = (head_ / SEG_RECORDS + 1) * SEG_RECORDS;  // numbering keeps going up
    first_ = cursor_ = head_;
}

}  // namespace dlog

#pragma once
// Persistent record log used as the node's outbox: every telemetry sample and
// event is appended here first and uploaded oldest-first once the gateway
// acknowledges it. A link, gateway or server outage therefore loses nothing
// until the quota (weeks of data) is exceeded, and then only the oldest.
//
// Storage is a set of append-only segment files of fixed-size slots, which
// suits flash filesystems (no in-place rewrites) and makes a record's sequence
// number simply its position:  seq = segment * SEG_RECORDS + slot.
// The logic has no Arduino dependencies and is unit-tested on the host.
#include <stddef.h>
#include <stdint.h>

namespace dlog {

constexpr size_t SLOT = 48;             // bytes per record on disk
constexpr uint8_t MAX_DATA = SLOT - 5;  // magic, type, len ... crc16
constexpr uint32_t SEG_RECORDS = 256;   // 12 KiB per segment file
constexpr uint8_t MAGIC = 0xD7;

struct Record {
    uint32_t seq = 0;
    uint8_t type = 0;  // opaque to the log (the node stores proto::MsgType)
    uint8_t len = 0;
    uint8_t data[MAX_DATA] = {};
};

// Segment store. Implementations: LittleFS on the ESP32, RAM in the tests,
// later an SD card.
struct Storage {
    virtual ~Storage() {}
    virtual size_t list(uint32_t* segments, size_t max) = 0;  // existing segment numbers, any order
    virtual size_t size(uint32_t segment) = 0;                // bytes, 0 if missing
    virtual bool append(uint32_t segment, const uint8_t* data, size_t n) = 0;
    virtual bool read(uint32_t segment, size_t offset, uint8_t* data, size_t n) = 0;
    virtual void remove(uint32_t segment) = 0;
};

class Log {
   public:
    Log(Storage& storage, uint16_t maxSegments) : st_(storage), maxSeg_(maxSegments ? maxSegments : 1) {}

    // Scans the storage. `savedCursor` is the upload position remembered by the
    // caller (NVS); it may be stale by a few records, which only causes resends.
    void begin(uint32_t savedCursor);

    bool append(uint8_t type, const uint8_t* data, uint8_t len);

    // Oldest record not yet acknowledged. Unreadable slots are skipped.
    bool peek(Record& r);
    void ack(uint32_t seq);  // everything up to and including seq is delivered

    bool read(uint32_t seq, Record& r);  // any stored record, for dumps
    void clear();                        // delete everything

    uint32_t oldest() const { return first_; }
    uint32_t head() const { return head_; }      // seq the next record will get
    uint32_t cursor() const { return cursor_; }  // next record to upload
    uint32_t backlog() const { return head_ - cursor_; }
    uint32_t capacity() const { return (uint32_t)maxSeg_ * SEG_RECORDS; }

    uint32_t dropped = 0;      // overwritten before they were uploaded
    uint32_t corrupt = 0;      // unreadable slots skipped
    uint32_t writeErrors = 0;

   private:
    Storage& st_;
    uint16_t maxSeg_;
    uint32_t first_ = 0, head_ = 0, cursor_ = 0;
};

}  // namespace dlog

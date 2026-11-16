// Host unit tests for the record log / outbox:  pio test -e native
#include <string.h>
#include <unity.h>

#include <map>
#include <vector>

#include "datalog.h"

using namespace dlog;

struct RamStorage : Storage {
    std::map<uint32_t, std::vector<uint8_t>> files;
    int failAppends = 0;     // next N appends write nothing and fail
    int partialAppends = 0;  // next N appends write half a slot and fail

    size_t list(uint32_t* segs, size_t max) override {
        size_t n = 0;
        for (auto& f : files)
            if (n < max) segs[n++] = f.first;
        return n;
    }
    size_t size(uint32_t seg) override { return files.count(seg) ? files[seg].size() : 0; }
    bool append(uint32_t seg, const uint8_t* d, size_t n) override {
        if (failAppends) return failAppends--, false;
        if (partialAppends) {
            partialAppends--;
            files[seg].insert(files[seg].end(), d, d + n / 2);
            return false;
        }
        files[seg].insert(files[seg].end(), d, d + n);
        return true;
    }
    bool read(uint32_t seg, size_t off, uint8_t* d, size_t n) override {
        if (!files.count(seg) || off + n > files[seg].size()) return false;
        memcpy(d, files[seg].data() + off, n);
        return true;
    }
    void remove(uint32_t seg) override { files.erase(seg); }
};

void setUp() {}
void tearDown() {}

// Record n carries its own number so order and content can be checked.
static bool put(Log& log, uint32_t n, uint8_t type = 0x10) {
    uint8_t d[8];
    memcpy(d, &n, 4);
    memset(d + 4, (int)(n & 0xFF), 4);
    return log.append(type, d, sizeof(d));
}
static uint32_t valueOf(const Record& r) {
    uint32_t n;
    memcpy(&n, r.data, 4);
    return n;
}

static void test_append_peek_ack_in_order() {
    RamStorage st;
    Log log(st, 4);
    log.begin(0);
    Record r;
    TEST_ASSERT_FALSE(log.peek(r));
    for (uint32_t i = 0; i < 5; i++) TEST_ASSERT_TRUE(put(log, i, (uint8_t)(0x10 + i)));
    TEST_ASSERT_EQUAL(5, log.backlog());
    for (uint32_t i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(log.peek(r));
        TEST_ASSERT_EQUAL(i, r.seq);
        TEST_ASSERT_EQUAL(i, valueOf(r));
        TEST_ASSERT_EQUAL(0x10 + i, r.type);
        TEST_ASSERT_EQUAL(8, r.len);
        TEST_ASSERT_TRUE(log.peek(r));  // peek does not consume
        TEST_ASSERT_EQUAL(i, r.seq);
        log.ack(r.seq);
    }
    TEST_ASSERT_FALSE(log.peek(r));
    TEST_ASSERT_EQUAL(0, log.backlog());
}

static void test_rejects_oversized_and_takes_max() {
    RamStorage st;
    Log log(st, 4);
    log.begin(0);
    uint8_t big[MAX_DATA + 1] = {};
    TEST_ASSERT_FALSE(log.append(1, big, MAX_DATA + 1));
    big[MAX_DATA - 1] = 0x5A;
    TEST_ASSERT_TRUE(log.append(1, big, MAX_DATA));
    Record r;
    TEST_ASSERT_TRUE(log.peek(r));
    TEST_ASSERT_EQUAL(MAX_DATA, r.len);
    TEST_ASSERT_EQUAL_HEX8(0x5A, r.data[MAX_DATA - 1]);
}

static void test_survives_restart_with_stale_cursor() {
    RamStorage st;
    {
        Log log(st, 4);
        log.begin(0);
        for (uint32_t i = 0; i < 300; i++) put(log, i);  // crosses a segment boundary
        Record r;
        for (int i = 0; i < 280; i++) {
            log.peek(r);
            log.ack(r.seq);
        }
        TEST_ASSERT_EQUAL(280, log.cursor());
    }
    Log log(st, 4);
    log.begin(272);  // the saved cursor lags: a few records are sent twice, none lost
    TEST_ASSERT_EQUAL(300, log.head());
    TEST_ASSERT_EQUAL(28, log.backlog());
    Record r;
    TEST_ASSERT_TRUE(log.peek(r));
    TEST_ASSERT_EQUAL(272, valueOf(r));
    put(log, 300);
    TEST_ASSERT_TRUE(log.read(300, r));
    TEST_ASSERT_EQUAL(300, valueOf(r));
}

static void test_quota_drops_oldest_and_counts() {
    RamStorage st;
    Log log(st, 2);  // room for 512 records
    log.begin(0);
    for (uint32_t i = 0; i < 3 * SEG_RECORDS + 10; i++) TEST_ASSERT_TRUE(put(log, i));
    TEST_ASSERT_EQUAL(2, st.files.size());
    TEST_ASSERT_EQUAL(2 * SEG_RECORDS, log.oldest());
    TEST_ASSERT_EQUAL(2 * SEG_RECORDS, log.dropped);  // never uploaded, overwritten
    Record r;
    TEST_ASSERT_TRUE(log.peek(r));
    TEST_ASSERT_EQUAL(2 * SEG_RECORDS, valueOf(r));
    TEST_ASSERT_EQUAL(SEG_RECORDS + 10, log.backlog());
}

static void test_delivered_data_is_not_counted_as_dropped() {
    RamStorage st;
    Log log(st, 2);
    log.begin(0);
    Record r;
    for (uint32_t i = 0; i < 4 * SEG_RECORDS; i++) {
        put(log, i);
        TEST_ASSERT_TRUE(log.peek(r));
        log.ack(r.seq);
    }
    TEST_ASSERT_EQUAL(0, log.dropped);
    TEST_ASSERT_EQUAL(0, log.backlog());
}

static void test_torn_last_write_starts_new_segment() {
    RamStorage st;
    {
        Log log(st, 4);
        log.begin(0);
        for (uint32_t i = 0; i < 10; i++) put(log, i);
    }
    st.files[0].resize(st.files[0].size() - 20);  // power cut in the middle of record 9
    Log log(st, 4);
    log.begin(0);
    TEST_ASSERT_EQUAL(SEG_RECORDS, log.head());
    put(log, 1000);
    Record r;
    uint32_t got = 0;
    while (log.peek(r)) {
        TEST_ASSERT_EQUAL(got < 9 ? got : 1000, valueOf(r));
        got++;
        log.ack(r.seq);
    }
    TEST_ASSERT_EQUAL(10, got);  // 0..8 and the new one; the torn record is gone
    TEST_ASSERT_TRUE(log.corrupt > 0);
}

static void test_corrupt_record_is_skipped() {
    RamStorage st;
    Log log(st, 4);
    log.begin(0);
    for (uint32_t i = 0; i < 5; i++) put(log, i);
    st.files[0][2 * SLOT + 7] ^= 0x01;
    Record r;
    uint32_t expect[] = {0, 1, 3, 4};
    for (uint32_t e : expect) {
        TEST_ASSERT_TRUE(log.peek(r));
        TEST_ASSERT_EQUAL(e, valueOf(r));
        log.ack(r.seq);
    }
    TEST_ASSERT_FALSE(log.peek(r));
    TEST_ASSERT_EQUAL(1, log.corrupt);
    TEST_ASSERT_FALSE(log.read(2, r));
}

static void test_write_failures() {
    RamStorage st;
    Log log(st, 4);
    log.begin(0);
    put(log, 0);
    st.failAppends = 1;  // nothing written: the same slot is used next time
    TEST_ASSERT_FALSE(put(log, 1));
    TEST_ASSERT_EQUAL(1, log.head());
    TEST_ASSERT_TRUE(put(log, 1));
    TEST_ASSERT_EQUAL(2, log.head());
    st.partialAppends = 1;  // half a slot written: the rest of the segment is abandoned
    TEST_ASSERT_FALSE(put(log, 2));
    TEST_ASSERT_EQUAL(SEG_RECORDS, log.head());
    TEST_ASSERT_TRUE(put(log, 2));
    TEST_ASSERT_EQUAL(2, log.writeErrors);
    Record r;
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(log.peek(r));
        TEST_ASSERT_EQUAL(i, valueOf(r));
        log.ack(r.seq);
    }
    TEST_ASSERT_FALSE(log.peek(r));
}

static void test_empty_storage_keeps_numbering_ahead_of_saved_cursor() {
    RamStorage st;  // e.g. the filesystem was reformatted but NVS kept the cursor
    Log log(st, 4);
    log.begin(1000);
    TEST_ASSERT_EQUAL(1024, log.head());
    TEST_ASSERT_EQUAL(0, log.backlog());
    put(log, 7);
    Record r;
    TEST_ASSERT_TRUE(log.peek(r));
    TEST_ASSERT_EQUAL(1024, r.seq);
}

static void test_stray_old_segments_are_removed_at_begin() {
    RamStorage st;
    {
        Log log(st, 8);
        log.begin(0);
        for (uint32_t i = 0; i < 6 * SEG_RECORDS; i++) put(log, i);
    }
    Log log(st, 2);  // quota reduced by a firmware update
    log.begin(0);
    TEST_ASSERT_EQUAL(2, st.files.size());
    TEST_ASSERT_EQUAL(4 * SEG_RECORDS, log.oldest());
    TEST_ASSERT_EQUAL(4 * SEG_RECORDS, log.cursor());
}

static void test_clear() {
    RamStorage st;
    Log log(st, 4);
    log.begin(0);
    for (uint32_t i = 0; i < 300; i++) put(log, i);
    log.clear();
    TEST_ASSERT_EQUAL(0, st.files.size());
    TEST_ASSERT_EQUAL(0, log.backlog());
    put(log, 42);
    Record r;
    TEST_ASSERT_TRUE(log.peek(r));
    TEST_ASSERT_EQUAL(42, valueOf(r));
    TEST_ASSERT_TRUE(r.seq >= 300);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_append_peek_ack_in_order);
    RUN_TEST(test_rejects_oversized_and_takes_max);
    RUN_TEST(test_survives_restart_with_stale_cursor);
    RUN_TEST(test_quota_drops_oldest_and_counts);
    RUN_TEST(test_delivered_data_is_not_counted_as_dropped);
    RUN_TEST(test_torn_last_write_starts_new_segment);
    RUN_TEST(test_corrupt_record_is_skipped);
    RUN_TEST(test_write_failures);
    RUN_TEST(test_empty_storage_keeps_numbering_ahead_of_saved_cursor);
    RUN_TEST(test_stray_old_segments_are_removed_at_begin);
    RUN_TEST(test_clear);
    return UNITY_END();
}

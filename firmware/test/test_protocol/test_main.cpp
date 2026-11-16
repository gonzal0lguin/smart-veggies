// Host unit tests for the link-layer frame codec:  pio test -e native
#include <string.h>
#include <unity.h>

#include "frame.h"

using namespace proto;

void setUp() {}
void tearDown() {}

static Frame sample(uint8_t len) {
    Frame f;
    f.dst = ADDR_GATEWAY;
    f.src = ADDR_NODE_1;
    f.type = MSG_PING;
    f.seq = 42;
    f.len = len;
    for (uint8_t i = 0; i < len; i++) f.payload[i] = (uint8_t)(i * 7 + 3);
    return f;
}

// Feeds bytes, returns how many frames came out; the last one is left in `out`.
static int feedAll(Parser& p, const uint8_t* data, size_t n, Frame& out) {
    int frames = 0;
    for (size_t i = 0; i < n; i++)
        if (p.feed(data[i], out)) frames++;
    return frames;
}

static void test_crc_known_vector() {
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16((const uint8_t*)"123456789", 9));
}

static void test_roundtrip_all_lengths() {
    for (size_t len = 0; len <= MAX_PAYLOAD; len++) {
        Frame in = sample((uint8_t)len), out;
        uint8_t buf[MAX_FRAME];
        size_t n = encode(in, buf, sizeof(buf));
        TEST_ASSERT_EQUAL(OVERHEAD + len, n);
        Parser p;
        TEST_ASSERT_EQUAL(1, feedAll(p, buf, n, out));
        TEST_ASSERT_EQUAL(in.dst, out.dst);
        TEST_ASSERT_EQUAL(in.src, out.src);
        TEST_ASSERT_EQUAL(in.type, out.type);
        TEST_ASSERT_EQUAL(in.seq, out.seq);
        TEST_ASSERT_EQUAL(in.len, out.len);
        if (len) TEST_ASSERT_EQUAL_MEMORY(in.payload, out.payload, len);
        TEST_ASSERT_TRUE(p.idle());
    }
}

static void test_encode_rejects_bad_sizes() {
    Frame f = sample(10);
    uint8_t buf[MAX_FRAME];
    TEST_ASSERT_EQUAL(0, encode(f, buf, OVERHEAD + 9));
    f.len = MAX_PAYLOAD + 1;
    TEST_ASSERT_EQUAL(0, encode(f, buf, sizeof(buf)));
}

static void test_max_frame_fits_e32_packet() {
    TEST_ASSERT_TRUE(MAX_FRAME <= 58);
}

static void test_corrupted_frame_is_dropped() {
    Frame in = sample(12), out;
    uint8_t buf[MAX_FRAME];
    size_t n = encode(in, buf, sizeof(buf));
    for (size_t i = 2; i < n; i++) {  // flip one bit in every byte after LEN
        uint8_t bad[MAX_FRAME];
        memcpy(bad, buf, n);
        bad[i] ^= 0x10;
        Parser p;
        TEST_ASSERT_EQUAL(0, feedAll(p, bad, n, out));
        TEST_ASSERT_EQUAL(1, p.crcErrors());
    }
}

static void test_resync_after_noise_and_back_to_back() {
    Frame a = sample(5), b = sample(20), out;
    b.seq = 43;
    uint8_t stream[2 * MAX_FRAME + 8];
    size_t n = 0;
    const uint8_t noise[] = {0x00, 0x13, 0xFF, 0x7E};  // no SYNC byte in here
    memcpy(stream, noise, sizeof(noise));
    n += sizeof(noise);
    n += encode(a, stream + n, sizeof(stream) - n);
    n += encode(b, stream + n, sizeof(stream) - n);
    Parser p;
    TEST_ASSERT_EQUAL(2, feedAll(p, stream, n, out));
    TEST_ASSERT_EQUAL(43, out.seq);
    TEST_ASSERT_EQUAL(20, out.len);
}

static void test_invalid_length_resyncs() {
    Frame in = sample(3), out;
    uint8_t stream[MAX_FRAME + 4];
    stream[0] = SYNC;
    stream[1] = 200;  // impossible length -> parser must drop it and look for SYNC again
    size_t n = 2 + encode(in, stream + 2, sizeof(stream) - 2);
    Parser p;
    TEST_ASSERT_EQUAL(1, feedAll(p, stream, n, out));
    // SYNC immediately followed by a real frame start
    stream[1] = SYNC;
    n = 1 + encode(in, stream + 1, sizeof(stream) - 1);
    Parser q;
    TEST_ASSERT_EQUAL(1, feedAll(q, stream, n, out));
}

static void test_reset_drops_partial_frame() {
    Frame in = sample(8), out;
    uint8_t buf[MAX_FRAME];
    size_t n = encode(in, buf, sizeof(buf));
    Parser p;
    TEST_ASSERT_EQUAL(0, feedAll(p, buf, n / 2, out));
    TEST_ASSERT_FALSE(p.idle());
    p.reset();
    TEST_ASSERT_EQUAL(1, feedAll(p, buf, n, out));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_known_vector);
    RUN_TEST(test_roundtrip_all_lengths);
    RUN_TEST(test_encode_rejects_bad_sizes);
    RUN_TEST(test_max_frame_fits_e32_packet);
    RUN_TEST(test_corrupted_frame_is_dropped);
    RUN_TEST(test_resync_after_noise_and_back_to_back);
    RUN_TEST(test_invalid_length_resyncs);
    RUN_TEST(test_reset_drops_partial_frame);
    return UNITY_END();
}

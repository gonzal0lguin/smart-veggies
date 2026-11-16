// Host unit tests: message codecs + command authentication.  pio test -e native
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "messages.h"
#include "sha256.h"

using namespace proto;

void setUp() {}
void tearDown() {}

static void hex(const uint8_t* d, size_t n, char* out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_sha256_vectors() {
    uint8_t h[32];
    char s[65];
    Sha256 a;
    a.update((const uint8_t*)"abc", 3);
    a.finish(h);
    hex(h, 32, s);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", s);
    Sha256 b;  // 56-byte message: padding spills into a second block
    b.update((const uint8_t*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    b.finish(h);
    hex(h, 32, s);
    TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", s);
}

static void test_hmac_rfc4231_case2() {
    uint8_t h[32];
    char s[65];
    const char* msg = "what do ya want for nothing?";
    hmacSha256((const uint8_t*)"Jefe", 4, (const uint8_t*)msg, strlen(msg), h);
    hex(h, 32, s);
    TEST_ASSERT_EQUAL_STRING("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", s);
}

static void test_telemetry_roundtrip() {
    Telemetry in, out;
    in.epoch = 1794000000u;
    for (int i = 0; i < SOIL_CHANNELS; i++) in.soilHalfPct[i] = (uint8_t)(i * 15);
    in.soilTempC100[0] = 1834;
    in.soilTempC100[1] = -250;
    in.airTempC100 = -1234;
    in.airRhHalfPct = 131;
    in.supplyMv = 12180;
    for (int i = 0; i < ZONES; i++) in.todayDl[i] = (uint16_t)(1000 * i + 7);
    Frame f;
    encode(in, f);
    TEST_ASSERT_EQUAL(MSG_TELEMETRY, f.type);
    TEST_ASSERT_TRUE(decode(f, out));
    TEST_ASSERT_EQUAL_UINT32(in.epoch, out.epoch);
    TEST_ASSERT_EQUAL_MEMORY(in.soilHalfPct, out.soilHalfPct, SOIL_CHANNELS);
    TEST_ASSERT_EQUAL(-250, out.soilTempC100[1]);
    TEST_ASSERT_EQUAL(-1234, out.airTempC100);
    TEST_ASSERT_EQUAL(131, out.airRhHalfPct);
    TEST_ASSERT_EQUAL(12180, out.supplyMv);
    TEST_ASSERT_EQUAL(5007, out.todayDl[5]);
    // survives the wire codec too
    uint8_t buf[MAX_FRAME];
    size_t n = encode(f, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    f.len--;  // wrong length must be refused
    TEST_ASSERT_FALSE(decode(f, out));
}

static void test_telemetry_defaults_are_no_data() {
    Telemetry t;
    TEST_ASSERT_EQUAL(NO_DATA_U8, t.soilHalfPct[11]);
    TEST_ASSERT_EQUAL(NO_DATA_I16, t.soilTempC100[1]);
}

static void test_status_event_ack_time_roundtrip() {
    Frame f;
    Status s, s2;
    s.epoch = 1;
    s.uptimeS = 0xDEADBEEF;
    s.flags = ST_AUTO | ST_LEAK;
    s.activeZone = 3;
    s.zoneState[3] = 2;
    s.zoneFault[5] = 1;
    s.valveMask = 0x08;
    s.runMl = 123456;
    s.cmdCounter = 0x01020304;
    s.resetReason = 9;
    encode(s, f);
    TEST_ASSERT_TRUE(decode(f, s2));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, s2.uptimeS);
    TEST_ASSERT_EQUAL(3, s2.activeZone);
    TEST_ASSERT_EQUAL(2, s2.zoneState[3]);
    TEST_ASSERT_EQUAL(1, s2.zoneFault[5]);
    TEST_ASSERT_EQUAL_UINT32(123456, s2.runMl);
    TEST_ASSERT_EQUAL_UINT32(0x01020304, s2.cmdCounter);
    TEST_ASSERT_EQUAL(9, s2.resetReason);

    Event e, e2;
    e.epoch = 77;
    e.id = 513;
    e.code = 2;
    e.zone = 4;
    e.detail = 1;
    e.a = 4000;
    e.b = 250;
    encode(e, f);
    TEST_ASSERT_TRUE(decode(f, e2));
    TEST_ASSERT_EQUAL(513, e2.id);
    TEST_ASSERT_EQUAL_UINT32(4000, e2.a);
    TEST_ASSERT_EQUAL_UINT32(250, e2.b);
    TEST_ASSERT_FALSE(decode(f, s2));  // wrong type

    Ack a, a2;
    a.seq = 200;
    a.type = MSG_CMD;
    a.result = ACK_REPLAY;
    a.detail = 3;
    a.counter = 99;
    encode(a, f);
    TEST_ASSERT_TRUE(decode(f, a2));
    TEST_ASSERT_EQUAL(200, a2.seq);
    TEST_ASSERT_EQUAL(ACK_REPLAY, a2.result);
    TEST_ASSERT_EQUAL_UINT32(99, a2.counter);

    TimeSync t, t2;
    t.counter = 5;
    t.epoch = 1794000000u;
    t.tzMinutes = -180;
    encode(t, f);
    TEST_ASSERT_TRUE(decode(f, t2));
    TEST_ASSERT_EQUAL(-180, t2.tzMinutes);
    TEST_ASSERT_EQUAL_UINT32(1794000000u, t2.epoch);
}

static const uint8_t KEY[] = "0123456789abcdef";

static Frame signedCmd() {
    Cmd c;
    c.counter = 42;
    c.cmd = CMD_WATER_VOLUME;
    c.zone = 1;
    c.arg = 40;
    c.arg2 = 900;
    c.expiry = 1794000600u;
    Frame f;
    f.dst = ADDR_NODE_1;
    f.src = ADDR_GATEWAY;
    f.seq = 7;
    encode(c, f);
    TEST_ASSERT_TRUE(sign(f, KEY, 16));
    return f;
}

static void test_signed_cmd_roundtrip() {
    Frame f = signedCmd();
    Cmd c;
    TEST_ASSERT_FALSE(decode(f, c));  // still carries the MAC: must verify first
    TEST_ASSERT_TRUE(verify(f, KEY, 16));
    TEST_ASSERT_TRUE(decode(f, c));
    TEST_ASSERT_EQUAL_UINT32(42, c.counter);
    TEST_ASSERT_EQUAL(CMD_WATER_VOLUME, c.cmd);
    TEST_ASSERT_EQUAL(40, c.arg);
    TEST_ASSERT_EQUAL(900, c.arg2);
    TEST_ASSERT_EQUAL_UINT32(1794000600u, c.expiry);
}

static void test_tampering_is_rejected() {
    const Frame good = signedCmd();
    for (uint8_t i = 0; i < good.len; i++) {  // any payload or MAC bit
        Frame f = good;
        f.payload[i] ^= 0x01;
        TEST_ASSERT_FALSE(verify(f, KEY, 16));
        TEST_ASSERT_EQUAL(good.len, f.len);
    }
    Frame f = good;
    f.dst = 0x02;  // header is covered too
    TEST_ASSERT_FALSE(verify(f, KEY, 16));
    f = good;
    f.type = MSG_TIME;
    TEST_ASSERT_FALSE(verify(f, KEY, 16));
    f = good;
    f.seq++;
    TEST_ASSERT_FALSE(verify(f, KEY, 16));
    f = good;
    TEST_ASSERT_FALSE(verify(f, (const uint8_t*)"0123456789abcdeX", 16));
    f.len = 2;  // shorter than a MAC
    TEST_ASSERT_FALSE(verify(f, KEY, 16));
}

static void test_sign_needs_room() {
    Frame f;
    f.len = MAX_PAYLOAD - MAC_LEN + 1;
    TEST_ASSERT_FALSE(sign(f, KEY, 16));
    f.len = MAX_PAYLOAD - MAC_LEN;
    TEST_ASSERT_TRUE(sign(f, KEY, 16));
    TEST_ASSERT_EQUAL(MAX_PAYLOAD, f.len);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_hmac_rfc4231_case2);
    RUN_TEST(test_telemetry_roundtrip);
    RUN_TEST(test_telemetry_defaults_are_no_data);
    RUN_TEST(test_status_event_ack_time_roundtrip);
    RUN_TEST(test_signed_cmd_roundtrip);
    RUN_TEST(test_tampering_is_rejected);
    RUN_TEST(test_sign_needs_room);
    return UNITY_END();
}

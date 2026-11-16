// TEST: E32 LoRa link + range test
//   ./scripts/test.sh lora_range          (flash the SAME firmware on both ESP32s)
//
// One board is "pong" (stays in the house, where the gateway will live) and
// the other is "ping" (walk it to the garden on a power bank). Set the role
// once over serial; it is remembered:
//     role pong        /        role ping
//
// The ping side sends a frame every `period` ms and waits for the echo.
// You can read the result without a laptop from the on-board LED:
//     ping:  short blink = round trip OK      solid ON = last ping was lost
//     pong:  short blink = ping received
// With a laptop, `stats` prints loss and round-trip times.
//
// Type `help` for commands. Radio settings (rate/chan/power) are stored in
// the module and MUST match on both boards.
#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "e32.h"
#include "frame.h"
#include "serial_cmd.h"

static E32 radio(Serial2, PIN_LORA_RX, PIN_LORA_TX, PIN_LORA_MODE, PIN_LORA_AUX);
static proto::Parser parser;
static LineReader console;
static Preferences prefs;

static bool isPing = false;
static uint16_t payloadSize = 16;
static uint32_t periodMs = 2000;
static bool radioOk = false;

struct Stats {
    uint32_t sent = 0, ok = 0, lost = 0, stray = 0;
    uint32_t rttMin = UINT32_MAX, rttMax = 0;
    uint64_t rttSum = 0;
    uint32_t consecLost = 0, maxConsecLost = 0;
    uint32_t rxPings = 0, rxGaps = 0;  // pong side
};
static Stats st;

static uint8_t seq = 0;
static bool waiting = false;
static uint32_t sentAt = 0, lastPingAt = 0, lastByteAt = 0, ledOffAt = 0;
static int lastRxSeq = -1;

static void led(bool on) { digitalWrite(PIN_LED, on ? HIGH : LOW); }
static void blink(uint32_t ms) {
    led(true);
    ledOffAt = millis() + ms;
}

static void sendFrame(uint8_t type, uint8_t s, const uint8_t* payload, uint8_t len) {
    proto::Frame f;
    f.dst = isPing ? proto::ADDR_GATEWAY : proto::ADDR_NODE_1;
    f.src = isPing ? proto::ADDR_NODE_1 : proto::ADDR_GATEWAY;
    f.type = type;
    f.seq = s;
    f.len = len;
    if (payload) memcpy(f.payload, payload, len);
    uint8_t buf[proto::MAX_FRAME];
    size_t n = proto::encode(f, buf, sizeof(buf));
    radio.send(buf, n);
}

static void printStats() {
    if (isPing) {
        float loss = st.sent ? 100.0f * st.lost / st.sent : 0;
        Serial.printf("PING stats: sent=%u ok=%u lost=%u (%.1f%%) worst streak=%u stray=%u crcErr=%u\n", st.sent,
                      st.ok, st.lost, loss, st.maxConsecLost, st.stray, parser.crcErrors());
        if (st.ok)
            Serial.printf("            round trip min/avg/max = %u / %u / %u ms\n", st.rttMin,
                          (uint32_t)(st.rttSum / st.ok), st.rttMax);
    } else {
        Serial.printf("PONG stats: pings received=%u, missing (by seq gaps)=%u, crcErr=%u\n", st.rxPings, st.rxGaps,
                      parser.crcErrors());
    }
}

static void printConfig() {
    E32::Config cfg;
    if (!radio.readConfig(cfg)) {
        Serial.println("E32: no answer in config mode. Check wiring: TXD->GPIO16, RXD<-GPIO17, M0+M1->GPIO25, "
                       "AUX->GPIO27, VCC, GND.");
        radioOk = false;
        return;
    }
    radioOk = true;
    E32::describe(cfg, Serial);
}

static void help() {
    Serial.println("commands:");
    Serial.println("  role ping|pong   set this board's role (saved)");
    Serial.println("  stats | reset    show / clear statistics");
    Serial.println("  size <0-48>      ping payload bytes (default 16; ~40 mimics real telemetry)");
    Serial.println("  period <ms>      ping interval (default 2000; use >=6000 at air rate 0)");
    Serial.println("  cfg              read the E32 configuration");
    Serial.println("  rate <0-5>       air rate 0=0.3k 1=1.2k 2=2.4k 3=4.8k 4=9.6k 5=19.2k (lower = more range)");
    Serial.println("  chan <0-31>      channel, frequency = 410 + chan MHz (default 23 = 433 MHz)");
    Serial.println("  power <0-3>      0=20dBm 1=17dBm 2=14dBm 3=10dBm");
    Serial.println("  defaults         restore factory radio settings (9600 8N1, 2.4k, 433 MHz, 20 dBm)");
}

static void changeRadio(const char* what, long value) {
    E32::Config cfg;
    if (!radio.readConfig(cfg)) {
        Serial.println("E32 not answering");
        return;
    }
    if (!strcmp(what, "rate") && value >= 0 && value <= 5)
        cfg.setAirRate((uint8_t)value);
    else if (!strcmp(what, "chan") && value >= 0 && value <= 31)
        cfg.chan = (uint8_t)value;
    else if (!strcmp(what, "power") && value >= 0 && value <= 3)
        cfg.setPower((uint8_t)value);
    else if (!strcmp(what, "defaults"))
        cfg = E32::Config();
    else {
        Serial.println("value out of range");
        return;
    }
    Serial.println(radio.writeConfig(cfg) ? "saved to module:" : "WRITE FAILED, module now has:");
    printConfig();
    Serial.println("(remember: the other board needs the same settings)");
}

static void handleLine(const char* line) {
    char cmd[16] = {}, arg[16] = {};
    int n = sscanf(line, "%15s %15s", cmd, arg);
    if (n < 1) return;
    long v = n > 1 ? strtol(arg, nullptr, 10) : -1;
    if (!strcmp(cmd, "help")) {
        help();
    } else if (!strcmp(cmd, "role") && n > 1 && (!strcmp(arg, "ping") || !strcmp(arg, "pong"))) {
        isPing = !strcmp(arg, "ping");
        prefs.putBool("ping", isPing);
        st = Stats();
        waiting = false;
        lastRxSeq = -1;
        led(false);
        Serial.printf("role = %s (saved)\n", isPing ? "PING" : "PONG");
    } else if (!strcmp(cmd, "stats")) {
        printStats();
    } else if (!strcmp(cmd, "reset")) {
        st = Stats();
        lastRxSeq = -1;
        Serial.println("stats cleared");
    } else if (!strcmp(cmd, "size") && v >= 0 && v <= (long)proto::MAX_PAYLOAD) {
        payloadSize = (uint16_t)v;
        Serial.printf("payload = %u bytes (frame = %u bytes)\n", payloadSize, payloadSize + (unsigned)proto::OVERHEAD);
    } else if (!strcmp(cmd, "period") && v >= 300) {
        periodMs = (uint32_t)v;
        Serial.printf("period = %u ms\n", periodMs);
    } else if (!strcmp(cmd, "cfg")) {
        printConfig();
    } else if (!strcmp(cmd, "rate") || !strcmp(cmd, "chan") || !strcmp(cmd, "power") || !strcmp(cmd, "defaults")) {
        changeRadio(cmd, v);
    } else {
        Serial.println("? (type help)");
    }
}

static void onFrame(const proto::Frame& f) {
    uint32_t now = millis();
    if (!isPing && f.type == proto::MSG_PING) {
        st.rxPings++;
        if (lastRxSeq >= 0) st.rxGaps += (uint8_t)(f.seq - (uint8_t)lastRxSeq - 1);
        lastRxSeq = f.seq;
        sendFrame(proto::MSG_PONG, f.seq, f.payload, f.len);
        blink(60);
        Serial.printf("ping #%u (%u bytes) -> pong   total=%u missing=%u\n", f.seq, f.len, st.rxPings, st.rxGaps);
    } else if (isPing && f.type == proto::MSG_PONG) {
        if (!waiting || f.seq != seq) {
            st.stray++;  // late answer to an earlier ping
            return;
        }
        waiting = false;
        uint32_t rtt = now - sentAt;
        st.ok++;
        st.consecLost = 0;
        st.rttSum += rtt;
        if (rtt < st.rttMin) st.rttMin = rtt;
        if (rtt > st.rttMax) st.rttMax = rtt;
        blink(60);
        Serial.printf("#%-3u OK   rtt=%4u ms   loss %u/%u (%.1f%%)\n", seq, rtt, st.lost, st.sent,
                      100.0f * st.lost / st.sent);
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    led(false);
    delay(500);
    Serial.println("\n=== LORA RANGE TEST ===");
    prefs.begin("range", false);
    isPing = prefs.getBool("ping", false);

    radioOk = radio.begin();
    if (radioOk) {
        uint8_t ver[4];
        if (radio.readVersion(ver))
            Serial.printf("E32 found: model 0x%02X version 0x%02X features 0x%02X\n", ver[1], ver[2], ver[3]);
    }
    printConfig();
    Serial.printf("role = %s   (change with: role ping | role pong)\n", isPing ? "PING" : "PONG");
    help();
}

void loop() {
    uint32_t now = millis();

    if (const char* line = console.poll()) handleLine(line);

    while (radio.available()) {
        proto::Frame f;
        lastByteAt = now;
        if (parser.feed((uint8_t)radio.read(), f)) onFrame(f);
    }
    // A frame arrives as one burst; drop leftovers after a silent gap.
    if (!parser.idle() && now - lastByteAt > 300) parser.reset();

    if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) {
        ledOffAt = 0;
        led(false);
    }

    if (isPing && radioOk) {
        // Give up on the previous ping shortly before the next one is due.
        if (waiting && now - sentAt > periodMs - 100) {
            waiting = false;
            st.lost++;
            st.consecLost++;
            if (st.consecLost > st.maxConsecLost) st.maxConsecLost = st.consecLost;
            ledOffAt = 0;
            led(true);  // solid = lost
            Serial.printf("#%-3u LOST             loss %u/%u (%.1f%%)\n", seq, st.lost, st.sent,
                          100.0f * st.lost / st.sent);
        }
        if (!waiting && now - lastPingAt >= periodMs) {
            lastPingAt = now;
            seq++;
            uint8_t payload[proto::MAX_PAYLOAD];
            for (uint16_t i = 0; i < payloadSize; i++) payload[i] = (uint8_t)(seq + i);
            sendFrame(proto::MSG_PING, seq, payload, (uint8_t)payloadSize);
            sentAt = millis();
            waiting = true;
            st.sent++;
        }
    }
    delay(2);
}

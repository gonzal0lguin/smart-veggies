#include "e32.h"

E32::E32(HardwareSerial& serial, int8_t rxPin, int8_t txPin, int8_t modePin, int8_t auxPin)
    : serial_(serial), rx_(rxPin), tx_(txPin), mode_(modePin), aux_(auxPin) {}

bool E32::begin() {
    pinMode(mode_, OUTPUT);
    digitalWrite(mode_, LOW);
    if (aux_ >= 0) pinMode(aux_, INPUT_PULLUP);
    serial_.begin(9600, SERIAL_8N1, rx_, tx_);
    delay(100);
    waitIdle(1000);
    Config cfg;
    return readConfig(cfg);
}

bool E32::waitIdle(uint32_t timeoutMs) {
    if (aux_ < 0) {
        delay(timeoutMs < 100 ? timeoutMs : 100);
        return true;
    }
    uint32_t start = millis();
    while (digitalRead(aux_) == LOW) {
        if (millis() - start > timeoutMs) return false;
        delay(1);
    }
    return true;
}

void E32::setConfigMode(bool on) {
    serial_.flush();
    waitIdle(1000);
    digitalWrite(mode_, on ? HIGH : LOW);
    delay(50);  // mode switch + module self-check; AUX goes low meanwhile
    waitIdle(1000);
    delay(20);
    while (serial_.available()) serial_.read();
}

bool E32::command(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen) {
    setConfigMode(true);
    serial_.write(tx, txLen);
    serial_.flush();
    size_t got = 0;
    uint32_t start = millis();
    while (got < rxLen && millis() - start < 600) {
        if (serial_.available())
            rx[got++] = (uint8_t)serial_.read();
        else
            delay(1);
    }
    setConfigMode(false);
    return got == rxLen;
}

bool E32::readConfig(Config& out) {
    const uint8_t cmd[3] = {0xC1, 0xC1, 0xC1};
    uint8_t r[6];
    if (!command(cmd, 3, r, 6)) return false;
    if (r[0] != 0xC0 && r[0] != 0xC2) return false;
    out.head = r[0];
    out.addh = r[1];
    out.addl = r[2];
    out.sped = r[3];
    out.chan = r[4];
    out.option = r[5];
    return true;
}

bool E32::writeConfig(const Config& cfg, bool save) {
    const uint8_t cmd[6] = {(uint8_t)(save ? 0xC0 : 0xC2), cfg.addh, cfg.addl, cfg.sped, cfg.chan, cfg.option};
    uint8_t echo[6];
    if (!command(cmd, 6, echo, 6)) return false;
    Config check;
    if (!readConfig(check)) return false;
    return check.addh == cfg.addh && check.addl == cfg.addl && check.sped == cfg.sped && check.chan == cfg.chan &&
           check.option == cfg.option;
}

bool E32::readVersion(uint8_t out[4]) {
    const uint8_t cmd[3] = {0xC3, 0xC3, 0xC3};
    return command(cmd, 3, out, 4) && out[0] == 0xC3;
}

size_t E32::send(const uint8_t* data, size_t len) {
    waitIdle(2000);
    return serial_.write(data, len);
}

const char* E32::airRateName(uint8_t rate) {
    static const char* names[] = {"0.3k", "1.2k", "2.4k", "4.8k", "9.6k", "19.2k", "19.2k", "19.2k"};
    return names[rate & 0x07];
}

const char* E32::powerName(uint8_t power) {
    static const char* names[] = {"20dBm", "17dBm", "14dBm", "10dBm"};
    return names[power & 0x03];
}

void E32::describe(const Config& c, Print& out) {
    out.printf("addr=0x%02X%02X  chan=%u (%d MHz)  air=%s (rate %u)  power=%s (%u)\n", c.addh, c.addl, c.chan,
               c.frequencyMHz(), airRateName(c.airRate()), c.airRate(), powerName(c.power()), c.power());
    out.printf("uart=%s  mode=%s  FEC=%s  raw: SPED=0x%02X OPTION=0x%02X\n",
               ((c.sped >> 3) & 0x1F) == 0x03 ? "9600 8N1" : "NOT 9600 8N1 (driver expects 9600 8N1!)",
               (c.option & 0x80) ? "fixed" : "transparent", (c.option & 0x04) ? "on" : "off", c.sped, c.option);
}

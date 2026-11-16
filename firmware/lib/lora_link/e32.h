#pragma once
// Driver for EBYTE E32-433T20D (SX1278 behind a UART) in transparent mode.
//
// Wiring: M0 and M1 are tied together to one GPIO. We only need two of the
// four modes: both low = normal (transparent TX/RX), both high = sleep/config.
// The UART stays at 9600 8N1, which is what config mode always uses.
#include <Arduino.h>

class E32 {
   public:
    struct Config {
        uint8_t head = 0xC0;
        uint8_t addh = 0;
        uint8_t addl = 0;
        uint8_t sped = 0x1A;    // 8N1, UART 9600, air 2.4 kbps
        uint8_t chan = 0x17;    // 410 + 23 = 433 MHz
        uint8_t option = 0x44;  // transparent, push-pull, 250 ms wake, FEC on, 20 dBm

        uint8_t airRate() const { return sped & 0x07; }
        void setAirRate(uint8_t r) { sped = (sped & 0xF8) | (r & 0x07); }
        uint8_t power() const { return option & 0x03; }
        void setPower(uint8_t p) { option = (option & 0xFC) | (p & 0x03); }
        int frequencyMHz() const { return 410 + (chan & 0x1F); }
    };

    E32(HardwareSerial& serial, int8_t rxPin, int8_t txPin, int8_t modePin, int8_t auxPin);

    // Starts the UART and reads the module configuration. False = no answer.
    bool begin();
    bool readConfig(Config& out);
    // save=true stores to the module's flash (survives power-off).
    bool writeConfig(const Config& cfg, bool save = true);
    bool readVersion(uint8_t out[4]);

    // Waits for AUX high (module idle). True immediately-ish when AUX is not wired.
    bool waitIdle(uint32_t timeoutMs);
    // Queues data for transmission. One call should be <= 58 bytes to stay in one air packet.
    size_t send(const uint8_t* data, size_t len);
    int available() { return serial_.available(); }
    int read() { return serial_.read(); }

    static const char* airRateName(uint8_t rate);
    static const char* powerName(uint8_t power);
    static void describe(const Config& cfg, Print& out);

   private:
    void setConfigMode(bool on);
    bool command(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t rxLen);

    HardwareSerial& serial_;
    int8_t rx_, tx_, mode_, aux_;
};

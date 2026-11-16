// TEST: capacitive soil moisture sensors (v1.2) — screening and calibration
//   ./scripts/test.sh soil
//
// Sensors powered from 3.3 V, outputs to ADS1115 inputs (or, with no ADS1115
// connected, to GPIO36/GPIO39 two at a time).
//
// For every sensor check:
//   1. In air the reading is stable (noise of a few mV, not hundreds).
//   2. Dip it in a glass of water up to the line: the reading must DROP by
//      roughly 1 V and settle in about a second. A sensor that takes many
//      seconds to settle, or barely moves, is from a defective batch.
//   3. Type `dry` with all sensors in air, then `wet` with them in water:
//      the % column appears and `cal` prints the calibration table to note down.
// Unconnected inputs float and show garbage; `only <n>` limits the printout.
#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "serial_cmd.h"
#include "soil_moisture.h"

static SoilMoisture soil;
static LineReader console;
static uint8_t shown = 0;  // number of channels printed
static bool paused = false;

static const uint8_t adsAddresses[] = SOIL_ADS_ADDRESSES;
static const uint8_t adcPins[] = SOIL_FALLBACK_ADC_PINS;

struct Sample {
    float mean = 0, min = 0, max = 0;
    bool ok = false;
};

// 8 readings per channel: the mean, and min/max as a noise figure.
static Sample sample(uint8_t ch) {
    Sample s;
    float sum = 0;
    s.min = 1e9f;
    s.max = -1e9f;
    for (int i = 0; i < 8; i++) {
        float mv;
        if (!soil.readMilliVolts(ch, mv)) return s;
        sum += mv;
        if (mv < s.min) s.min = mv;
        if (mv > s.max) s.max = mv;
    }
    s.mean = sum / 8;
    s.ok = true;
    return s;
}

static void printCal() {
    Serial.println("ch  input      dry(mV)  wet(mV)  span(mV)");
    for (uint8_t ch = 0; ch < soil.channels(); ch++) {
        char name[12];
        soil.describe(ch, name, sizeof(name));
        auto& c = soil.calibration(ch);
        Serial.printf("%-3u %-9s  %7.0f  %7.0f  %7.0f %s\n", ch, name, c.dryMv, c.wetMv, c.dryMv - c.wetMv,
                      c.valid() ? "" : "(not calibrated)");
    }
}

static void capture(bool dry) {
    for (uint8_t ch = 0; ch < shown; ch++) {
        Sample s = sample(ch);
        if (!s.ok) continue;
        (dry ? soil.calibration(ch).dryMv : soil.calibration(ch).wetMv) = s.mean;
    }
    Serial.printf("captured %s point for channels 0..%u\n", dry ? "DRY (air)" : "WET (water)", shown - 1);
    printCal();
}

static void help() {
    Serial.println("commands: dry | wet | cal | only <n channels> | pause | help");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== SOIL MOISTURE TEST ===");
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    soil.begin(adsAddresses, sizeof(adsAddresses), adcPins, sizeof(adcPins));
    shown = soil.channels();
    if (soil.usingAds())
        Serial.printf("ADS1115 found: %u channels\n", soil.channels());
    else
        Serial.printf("No ADS1115 on I2C -> using ESP32 ADC pins (%u channels, less accurate)\n", soil.channels());
    help();
}

void loop() {
    static uint32_t lastPrint = 0;
    static uint32_t lines = 0;

    if (const char* line = console.poll()) {
        char cmd[16] = {};
        int v = -1;
        sscanf(line, "%15s %d", cmd, &v);
        if (!strcmp(cmd, "dry"))
            capture(true);
        else if (!strcmp(cmd, "wet"))
            capture(false);
        else if (!strcmp(cmd, "cal"))
            printCal();
        else if (!strcmp(cmd, "only") && v >= 1 && v <= soil.channels())
            shown = (uint8_t)v;
        else if (!strcmp(cmd, "pause"))
            paused = !paused;
        else
            help();
    }

    if (paused || millis() - lastPrint < 500) return;
    lastPrint = millis();

    if (lines++ % 20 == 0) {
        Serial.print("\n");
        for (uint8_t ch = 0; ch < shown; ch++) {
            char name[12];
            soil.describe(ch, name, sizeof(name));
            Serial.printf("| %u:%-12s ", ch, name);
        }
        Serial.println("|   mV ±noise  %");
    }
    for (uint8_t ch = 0; ch < shown; ch++) {
        Sample s = sample(ch);
        if (!s.ok) {
            Serial.print("|   read error   ");
            continue;
        }
        float pct = soil.toPercent(ch, s.mean);
        if (isnan(pct))
            Serial.printf("| %4.0f ±%-3.0f   --  ", s.mean, (s.max - s.min) / 2);
        else
            Serial.printf("| %4.0f ±%-3.0f  %3.0f%% ", s.mean, (s.max - s.min) / 2, pct);
    }
    Serial.println("|");
}

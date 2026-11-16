// TEST: I2C scan + DS18B20 soil temperature probes + DHT11 air temp/humidity
//   ./scripts/test.sh climate
//
// Check:
//   - the I2C scan lists what is connected (ADS1115 = 0x48..0x4B, DS3231 = 0x68,
//     INA219 = 0x40, SHT3x = 0x44, ...). Nothing connected = empty list, fine.
//   - every DS18B20 probe is found (needs the 4.7k pull-up on the data line).
//     Warm one in your hand to learn which address is which probe, and note
//     the addresses down: the firmware will identify probes by address.
//   - the DHT11 prints plausible values (it only updates every ~2 s).
#include <Arduino.h>
#include <DHTesp.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Wire.h>

#include "config.h"

static OneWire oneWire(PIN_ONEWIRE);
static DallasTemperature probes(&oneWire);
static DHTesp dht;

static void scanI2C() {
    Serial.print("I2C devices:");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf(" 0x%02X", addr);
            found++;
        }
    }
    Serial.println(found ? "" : " none");
}

static void printAddress(const DeviceAddress a) {
    for (int i = 0; i < 8; i++) Serial.printf("%02X", a[i]);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== CLIMATE / BUS TEST ===");
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    scanI2C();

    probes.begin();
    Serial.printf("DS18B20 probes on GPIO%d: %d\n", PIN_ONEWIRE, probes.getDeviceCount());
    if (probes.getDeviceCount() == 0) Serial.println("  none found: check the 4.7k pull-up from data to 3.3 V");

    dht.setup(PIN_DHT, DHTesp::DHT11);
    Serial.printf("DHT11 on GPIO%d\n\n", PIN_DHT);
}

void loop() {
    static uint32_t n = 0;

    // Re-scan the 1-Wire bus now and then so probes can be plugged in live.
    if (n++ % 10 == 0) probes.begin();

    probes.requestTemperatures();
    int count = probes.getDeviceCount();
    for (int i = 0; i < count; i++) {
        DeviceAddress addr;
        if (!probes.getAddress(addr, i)) continue;
        float t = probes.getTempC(addr);
        Serial.print("soil ");
        printAddress(addr);
        if (t == DEVICE_DISCONNECTED_C)
            Serial.print(": read error   ");
        else
            Serial.printf(": %6.2f C   ", t);
    }

    TempAndHumidity air = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE)
        Serial.printf("| air %.1f C  %.0f %%RH\n", air.temperature, air.humidity);
    else
        Serial.printf("| DHT: %s\n", dht.getStatusString());

    delay(2500);
}

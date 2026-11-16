#pragma once
// Copy to include/secrets.h (git-ignored) and fill in.

// Shared by node and gateway: authenticates commands over LoRa. Any string,
// 16+ random characters. Both boards must be built with the same value.
#define LINK_KEY "replace-with-16+-random-chars"

// Gateway only
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-wifi-password"
#define MQTT_HOST "192.168.1.10"   // the Raspberry Pi
#define MQTT_PORT 1883
#define MQTT_USER "garden"
#define MQTT_PASS "replace-me"
#define TZ_OFFSET_MINUTES 0        // local time = UTC + this (no automatic DST yet)

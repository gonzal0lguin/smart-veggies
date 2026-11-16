#pragma once
// =============================================================================
//  Hardware configuration — single source of truth for pins and constants.
//  Phase 1 (breadboard bring-up) pin map for an ESP32 NodeMCU-32S / DevKit.
//  Pins avoided on purpose: 0, 2, 5, 12, 15 (boot strapping), 6-11 (flash),
//  1/3 (USB serial). 34-39 are input-only without internal pull-ups.
// =============================================================================

// ------------------------------------------------------ LoRa E32-433T20D (UART2)
#define PIN_LORA_RX   16   // ESP RX2  <- E32 TXD
#define PIN_LORA_TX   17   // ESP TX2  -> E32 RXD
#define PIN_LORA_MODE 25   // -> E32 M0 and M1 tied together (0 = normal, 1 = config/sleep)
#define PIN_LORA_AUX  27   // <- E32 AUX (low = busy). Set to -1 if not wired.

// ------------------------------------------------------------------------ I2C
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define I2C_FREQ_HZ 100000

// ------------------------------------------------- Soil moisture (capacitive)
// Preferred: ADS1115 modules on I2C (ADDR pin: GND=0x48, VDD=0x49, SDA=0x4A),
// 4 sensors each. If no ADS1115 answers, the soil test falls back to the two
// ESP32 ADC1 pins below so sensors can be screened before the ADCs arrive.
#define SOIL_ADS_ADDRESSES {0x48, 0x49, 0x4A}
#define SOIL_FALLBACK_ADC_PINS {39, 33}

// ------------------------------------------------------- Temperature sensors
#define PIN_ONEWIRE 4      // DS18B20 data, 4.7k pull-up to 3.3 V
#define PIN_DHT     14     // DHT11 data (module with pull-up, or 10k to 3.3 V)

// ---------------------------------------------------------------- Flow sensors
// YF-S201 powered from 5 V: signal MUST go through a divider (10k series,
// 20k to GND) or a level shifter before reaching these 3.3 V pins.
// One sensor per bed: it feeds both half-bed valves (zones run one at a time).
#define FLOW_PINS {34, 35, 36}
#define FLOW_COUNT 3
#define FLOW_PULSES_PER_LITRE 450.0f   // YF-S201 nominal (7.5 Hz per L/min); calibrate!

// --------------------------------------------------------------- Valve relays
// 6 zones = 3 beds x 2 halves (6-relay board). Zone z is fed through flow
// sensor ZONE_FLOW_SENSOR[z]. On the final IO board the relays move to an I2C
// expander (see docs/PLAN.md 4.10), which frees these GPIOs.
#define VALVE_PINS {18, 19, 23, 26, 32, 13}
#define VALVE_COUNT 6
#define ZONE_FLOW_SENSOR {0, 0, 1, 1, 2, 2}
// Most opto-isolated relay boards switch ON when the input is pulled LOW.
#define VALVE_RELAY_ACTIVE_LOW 1

// ------------------------------------------------------------- Zones (defaults)
// Which zone each moisture channel sits in (used for "skip if already wet").
#define SOIL_CHANNEL_ZONE {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5}
#define SOIL_TEMP_COUNT 2
#define ZONE_MAX_RUN_S 1800        // hard cap per run
#define ZONE_MAX_RUN_L 30          // hard cap per run
#define ZONE_MAX_DAILY_L 120       // hard cap per day
#define ZONE_NOMINAL_ML_MIN 1000   // expected drip flow per zone; re-measure in Phase 1
#define ZONE_MIN_ML_MIN 100        // less than this while watering = no-flow fault
#define ZONE_MAX_ML_MIN 6000       // more than this = burst fault

// ------------------------------------------------------------------- LoRa link
#define NODE_ADDRESS 0x01
#define TELEMETRY_PERIOD_S 60
#define STATUS_PERIOD_S 300
#define LINK_RETRY_MS 4000         // > air time of frame + ack at the chosen air rate
#define LINK_ATTEMPTS 4
// Shared secret for command authentication. Override in include/secrets.h
// (not committed); the default only exists so a fresh checkout builds.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef LINK_KEY
#define LINK_KEY "change-me-16byte"
#define LINK_KEY_IS_DEFAULT 1
#endif

// ------------------------------------------------------------------------ Misc
#define PIN_LED 2          // on-board LED (strapping pin, output only after boot)

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// TEST: valve relay outputs
//   ./scripts/test.sh valves
//
// Start with the relay board only (no valves, no water) and check:
//   - NO relay clicks while the ESP32 boots, resets or is being flashed.
//     If one does, the board's logic level is wrong for this wiring: flip
//     VALVE_RELAY_ACTIVE_LOW in include/config.h, or see docs/phase1-bringup.md.
//   - `on 1 5` clicks relay 1 on and it turns itself off after 5 s.
//   - `cycle` steps through all relays.
// Then connect a valve + 12 V and listen for it opening.
// Valves are numbered from 1. Every `on` has a time limit (default 10 s, max 600 s).
#include <Arduino.h>

#include "config.h"
#include "serial_cmd.h"
#include "valves.h"

static const int8_t valvePins[] = VALVE_PINS;
static ValveBank valves(valvePins, VALVE_COUNT, VALVE_RELAY_ACTIVE_LOW);
static LineReader console;

static int cycleNext = -1;
static uint32_t cycleAt = 0;

static void help() {
    Serial.println("commands:");
    Serial.println("  on <valve> [seconds]   open one valve (closes the others), default 10 s");
    Serial.println("  off [valve]            close one valve, or all");
    Serial.println("  cycle                  each valve 2 s on, one after another");
    Serial.println("  multi on|off           allow several valves open at once (default off)");
    Serial.println("  status");
}

static void status() {
    for (uint8_t i = 0; i < valves.count(); i++) {
        if (valves.isOpen(i))
            Serial.printf("  valve %u (GPIO%d): OPEN, %.1f s left\n", i + 1, valvePins[i], valves.remainingMs(i) / 1000.0f);
        else
            Serial.printf("  valve %u (GPIO%d): closed\n", i + 1, valvePins[i]);
    }
}

void setup() {
    valves.begin();  // first thing: outputs to a safe level
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== VALVE RELAY TEST ===");
    Serial.printf("%d outputs, relay board active %s\n", VALVE_COUNT, VALVE_RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
    help();
    status();
}

void loop() {
    static bool wasOpen[ValveBank::MAX_VALVES] = {};

    if (const char* line = console.poll()) {
        char cmd[16] = {}, arg[16] = {};
        int secs = 10;
        int n = sscanf(line, "%15s %15s %d", cmd, arg, &secs);
        int v = n > 1 ? atoi(arg) : 0;
        if (!strcmp(cmd, "on") && v >= 1 && v <= valves.count() && secs >= 1 && secs <= 600) {
            valves.open((uint8_t)(v - 1), (uint32_t)secs * 1000);
            Serial.printf("valve %d OPEN for %d s\n", v, secs);
        } else if (!strcmp(cmd, "off")) {
            cycleNext = -1;
            if (v >= 1 && v <= valves.count())
                valves.close((uint8_t)(v - 1));
            else
                valves.closeAll();
        } else if (!strcmp(cmd, "cycle")) {
            cycleNext = 0;
            cycleAt = millis();
        } else if (!strcmp(cmd, "multi") && n > 1) {
            valves.setExclusive(strcmp(arg, "on") != 0);
            Serial.printf("several valves at once: %s\n", strcmp(arg, "on") ? "no" : "yes");
        } else if (!strcmp(cmd, "status")) {
            status();
        } else {
            help();
        }
    }

    if (cycleNext >= 0 && (int32_t)(millis() - cycleAt) >= 0) {
        if (cycleNext < valves.count()) {
            valves.open((uint8_t)cycleNext, 2000);
            Serial.printf("cycle: valve %d\n", cycleNext + 1);
            cycleNext++;
            cycleAt = millis() + 2500;
        } else {
            cycleNext = -1;
            Serial.println("cycle done");
        }
    }

    valves.tick();
    for (uint8_t i = 0; i < valves.count(); i++) {
        if (wasOpen[i] && !valves.isOpen(i)) Serial.printf("valve %u closed\n", i + 1);
        wasOpen[i] = valves.isOpen(i);
    }
    delay(5);
}

// TEST: flow sensors (YF-S201) + dosing by volume
//   ./scripts/test.sh flow
//
// The question this test answers: can the YF-S201 measure the LOW flow of a
// drip line (around 1 L/min), and how repeatable is it?
//
// 1. Sanity: blow through a sensor, pulses must count up.
// 2. Calibrate at the real flow: sensor in line with the drip line (or a tap
//    throttled to a similar trickle) emptying into a measuring jug.
//       reset            -> zero the counters
//       open 1 120       -> valve 1 for 120 s (or just open the tap by hand)
//       cal 1 1.85       -> "sensor 1 really passed 1.85 L": computes pulses/litre
//    Repeat 3 times; the pulses/litre figure should agree within a few percent.
//    Also try half and double the flow: if the figure changes a lot, or the
//    sensor stops turning at low flow, it is not suitable for drip dosing.
// 3. dose 1 2.0        -> opens valve 1 until sensor 1 has counted 2.0 L
//    (stops on timeout, default 300 s, or if no flow is seen for 10 s).
// Sensors and valves are numbered from 1; valve N is measured by sensor N.
#include <Arduino.h>

#include "config.h"
#include "flow_meter.h"
#include "serial_cmd.h"
#include "valves.h"

static const uint8_t flowPins[] = FLOW_PINS;
static const int8_t valvePins[] = VALVE_PINS;
static FlowMeter flow[FLOW_COUNT];
static ValveBank valves(valvePins, VALVE_COUNT, VALVE_RELAY_ACTIVE_LOW);
static LineReader console;

struct Dose {
    bool active = false;
    uint8_t ch = 0;
    float targetL = 0, startL = 0;
    uint32_t startMs = 0, timeoutMs = 0, lastFlowMs = 0;
    uint32_t lastPulses = 0;
};
static Dose dose;

static void help() {
    Serial.println("commands:");
    Serial.println("  reset                        zero all counters");
    Serial.println("  open <valve> <seconds>       open a valve for a fixed time (max 600 s)");
    Serial.println("  dose <valve> <litres> [timeout_s]   open until that volume is measured");
    Serial.println("  stop                         close everything");
    Serial.println("  cal <sensor> <litres>        set pulses/litre from a measured volume since `reset`");
    Serial.println("  k <sensor> <pulses_per_litre>");
    Serial.println("  status");
}

static void status() {
    for (uint8_t i = 0; i < FLOW_COUNT; i++)
        Serial.printf("  sensor %u (GPIO%u): %6u pulses  %7.3f L  %5.1f Hz  %5.2f L/min   k=%.1f pulses/L   valve %s\n",
                      i + 1, flowPins[i], flow[i].pulses(), flow[i].litres(), flow[i].hz(),
                      flow[i].litresPerMinute(), flow[i].pulsesPerLitre(), valves.isOpen(i) ? "OPEN" : "closed");
}

static void endDose(const char* why) {
    valves.closeAll();
    float got = flow[dose.ch].litres() - dose.startL;
    Serial.printf("DOSE END (%s): %.3f L of %.3f L in %.1f s\n", why, got, dose.targetL,
                  (millis() - dose.startMs) / 1000.0f);
    Serial.println("  (watch the counter for a few seconds: litres still rising = overrun after closing)");
    dose.active = false;
}

static void handleLine(const char* line) {
    char cmd[16] = {};
    int idx = 0;
    float a = 0, b = 0;
    int n = sscanf(line, "%15s %d %f %f", cmd, &idx, &a, &b);
    bool idxOk = idx >= 1 && idx <= FLOW_COUNT;
    if (!strcmp(cmd, "reset")) {
        for (auto& f : flow) f.resetTotal();
        Serial.println("counters zeroed");
    } else if (!strcmp(cmd, "open") && idxOk && n >= 3 && a >= 1 && a <= 600) {
        valves.open((uint8_t)(idx - 1), (uint32_t)(a * 1000));
        Serial.printf("valve %d open for %.0f s\n", idx, a);
    } else if (!strcmp(cmd, "dose") && idxOk && n >= 3 && a > 0 && a <= 50) {
        dose.active = true;
        dose.ch = (uint8_t)(idx - 1);
        dose.targetL = a;
        dose.startL = flow[dose.ch].litres();
        dose.startMs = dose.lastFlowMs = millis();
        dose.lastPulses = flow[dose.ch].pulses();
        dose.timeoutMs = (uint32_t)((n >= 4 && b >= 5 && b <= 3600 ? b : 300) * 1000);
        valves.open(dose.ch, dose.timeoutMs + 1000);
        Serial.printf("DOSE: valve %d until %.3f L (timeout %u s)\n", idx, a, dose.timeoutMs / 1000);
    } else if (!strcmp(cmd, "stop")) {
        if (dose.active)
            endDose("stopped");
        else
            valves.closeAll();
        Serial.println("all closed");
    } else if (!strcmp(cmd, "cal") && idxOk && n >= 3 && a > 0) {
        FlowMeter& f = flow[idx - 1];
        if (f.pulses() < 50) {
            Serial.println("too few pulses since reset to calibrate");
        } else {
            float k = f.pulses() / a;
            Serial.printf("sensor %d: %u pulses / %.3f L = %.1f pulses/L (was %.1f)\n", idx, f.pulses(), a, k,
                          f.pulsesPerLitre());
            f.setPulsesPerLitre(k);
        }
    } else if (!strcmp(cmd, "k") && idxOk && n >= 3 && a > 0) {
        flow[idx - 1].setPulsesPerLitre(a);
        Serial.printf("sensor %d: k = %.1f pulses/L\n", idx, a);
    } else if (!strcmp(cmd, "status")) {
        status();
    } else {
        help();
    }
}

void setup() {
    valves.begin();
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== FLOW SENSOR TEST ===");
    for (uint8_t i = 0; i < FLOW_COUNT; i++) flow[i].begin(flowPins[i], FLOW_PULSES_PER_LITRE);
    help();
}

void loop() {
    static uint32_t lastPrint = 0;

    if (const char* line = console.poll()) handleLine(line);
    valves.tick();
    for (auto& f : flow) f.update();

    uint32_t now = millis();
    if (dose.active) {
        FlowMeter& f = flow[dose.ch];
        if (f.pulses() != dose.lastPulses) {
            dose.lastPulses = f.pulses();
            dose.lastFlowMs = now;
        }
        if (f.litres() - dose.startL >= dose.targetL)
            endDose("target reached");
        else if (now - dose.startMs > dose.timeoutMs)
            endDose("TIMEOUT");
        else if (now - dose.lastFlowMs > 10000)
            endDose("NO FLOW for 10 s");
    }

    if (now - lastPrint >= 1000) {
        lastPrint = now;
        for (uint8_t i = 0; i < FLOW_COUNT; i++) {
            if (flow[i].hz() > 0 || valves.isOpen(i))
                Serial.printf("sensor %u: %5.1f Hz  %5.2f L/min  total %7.3f L (%u pulses)%s\n", i + 1, flow[i].hz(),
                              flow[i].litresPerMinute(), flow[i].litres(), flow[i].pulses(),
                              valves.isOpen(i) ? "  [valve open]" : "");
        }
    }
    delay(2);
}

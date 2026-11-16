#!/usr/bin/env bash
# Build, flash and open the serial monitor for one module test firmware.
#
#   ./scripts/test.sh <test> [serial-port]
#   ./scripts/test.sh build-all          # just compile every test (no board needed)
set -euo pipefail
cd "$(dirname "$0")/.."

PIO="${PIO:-pio}"
command -v "$PIO" >/dev/null 2>&1 || PIO="$HOME/.platformio/penv/bin/pio"

TESTS="lora_range soil flow valves climate"

usage() {
    echo "usage: $0 <test> [port]   |   $0 build-all"
    echo
    echo "  lora_range   E32 LoRa: module config + ping/pong range test (flash on both ESP32s)"
    echo "  soil         capacitive moisture sensors: screening and dry/wet calibration"
    echo "  flow         flow sensors: pulses, calibration, dose-by-volume with a valve"
    echo "  valves       relay outputs: timed on/off, cycling"
    echo "  climate      I2C scan, DS18B20 probes, DHT11"
    exit 1
}

[ $# -ge 1 ] || usage

if [ "$1" = "build-all" ]; then
    for t in $TESTS; do
        echo "=== building $t"
        TEST=$t "$PIO" run -e test -s || { echo "FAILED: $t"; exit 1; }
    done
    for e in node node-sim; do
        echo "=== building app: $e"
        "$PIO" run -e "$e" -s || { echo "FAILED: $e"; exit 1; }
    done
    if [ -f include/secrets.h ]; then
        echo "=== building app: gateway"
        "$PIO" run -e gateway -s || { echo "FAILED: gateway"; exit 1; }
    else
        echo "=== skipping gateway (no include/secrets.h)"
    fi
    echo "all builds OK"
    exit 0
fi

TEST_NAME="$1"
case " $TESTS " in *" $TEST_NAME "*) ;; *) usage ;; esac

PORT_ARGS=()
if [ $# -ge 2 ]; then PORT_ARGS=(--upload-port "$2" --monitor-port "$2"); fi

TEST="$TEST_NAME" "$PIO" run -e test -t upload -t monitor "${PORT_ARGS[@]}"

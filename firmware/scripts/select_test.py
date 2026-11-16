# PlatformIO pre-build script for [env:test]: picks which test sketch to build
# from the TEST environment variable, e.g.  TEST=soil pio run -e test -t upload
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

TESTS = {
    "lora_range": "E32 LoRa link: module config + ping/pong packet-loss range test",
    "soil": "Capacitive moisture sensors: screening, noise, dry/wet calibration",
    "flow": "Flow sensors: pulse counting, calibration, dose-by-volume with a valve",
    "valves": "Relay outputs: timed on/off, boot state, cycling",
    "climate": "I2C scan, DS18B20 soil temperature probes, DHT11 air temp/humidity",
}

name = os.environ.get("TEST", "").strip().lower().replace("-", "_")
if name not in TESTS:
    lines = "\n".join(f"  {k:12s} {v}" for k, v in TESTS.items())
    print(f"\n*** Set TEST to one of:\n{lines}\n\nExample: TEST=soil pio run -e test -t upload -t monitor\n")
    env.Exit(1)  # noqa: F821

env.Replace(SRC_FILTER=["-<*>", f"+<tests/test_{name}.cpp>"])  # noqa: F821
print(f"Building test firmware: {name} - {TESTS[name]}")

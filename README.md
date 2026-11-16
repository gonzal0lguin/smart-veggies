# smart-veggies

Automated drip watering for a small vegetable nursery: an autonomous ESP32 field
node (valves, flow, soil and air sensors, on-site panel) linked over LoRa to a
gateway and a home server for logging, remote control and data-driven watering.

- [docs/PLAN.md](docs/PLAN.md) — architecture, decisions, roadmap; **starts with the current state and what to do next**
- [docs/phase1-bringup.md](docs/phase1-bringup.md) — bench tests, wiring, result tables (waiting for hardware)
- [docs/phase2-testing.md](docs/phase2-testing.md) — what is written but not yet proven on hardware, and how to check it
- [server/README.md](server/README.md) — server stack setup

```
docs/       plan and bring-up notes
firmware/   PlatformIO project (ESP32): lib/ modules, src/node + src/gateway apps, src/tests/ bring-up firmwares, host unit tests
hardware/   KiCad boards and enclosure (Phase 4)
server/     docker compose stack, garden-brain service (logging, garden map, forecast), fake node
```

## Firmware quick start

```bash
cd firmware
./scripts/test.sh build-all          # compile every bring-up firmware
./scripts/test.sh lora_range         # flash + monitor one of: lora_range soil flow valves climate
pio test -e native                   # unit tests on the PC: protocol, messages/auth, irrigation, scheduler, clock maths, data log
pio run -e node-sim -t upload -t monitor   # whole node on a bare ESP32 with simulated sensors
pio run -e node -t upload -t monitor       # real node
pio run -e gateway -t upload -t monitor    # LoRa <-> MQTT bridge (needs include/secrets.h)
```

## Automated checks

GitHub Actions (`.github/workflows/ci.yml`) runs on every push and pull request:

| Job | What it proves |
|---|---|
| Firmware logic | host unit tests (`pio test -e native`) |
| Firmware builds | every bring-up firmware, `node`, `node-sim` and `gateway` compile for the ESP32 |
| Server | compose file is valid; `server/tests/smoke_test.py` runs a broker, the fake node and garden-brain and checks a watering command end to end, the raw archive, backfilled data and the dataset export |

Hardware behaviour (radio, sensors, valves) cannot be checked there; that is what
the checklists in `docs/` are for.

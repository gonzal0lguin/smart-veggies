# smart-veggies

Automated drip watering for a small vegetable nursery: an autonomous ESP32 field
node (valves, flow, soil and air sensors, on-site panel) linked over LoRa to a
gateway and a home server for logging, remote control and data-driven watering.

- [docs/PLAN.md](docs/PLAN.md) — architecture, decisions, roadmap
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
pio test -e native                   # unit tests on the PC: protocol, messages/auth, irrigation, scheduler
pio run -e node-sim -t upload -t monitor   # whole node on a bare ESP32 with simulated sensors
pio run -e node -t upload -t monitor       # real node
pio run -e gateway -t upload -t monitor    # LoRa <-> MQTT bridge (needs include/secrets.h)
```

# Phase 2/3 software — what exists and how to prove it on hardware

Everything here was written before any hardware was available. Verified so far:
all firmwares compile, and the pure logic passes 40 unit tests on the PC
(`pio test -e native`): frame codec, message codecs, command authentication,
irrigation core, scheduler. **Nothing has run on an ESP32 yet**, and the gateway,
MQTT path and docker stack have never been run end to end.

Do the Phase 1 bench tests first ([phase1-bringup.md](phase1-bringup.md)); they
can change constants used here.

## Step 1 — node simulator on a bare ESP32 (needs only one board, no wiring)

`cd firmware && pio run -e node-sim -t upload -t monitor`

| Check | Expected | OK? |
|---|---|---|
| `water 1 2` | run started → finished, about 2.0 L in about 120 s | |
| `water 1 1`, `water 2 1`, `water 3 1` quickly | run one after another, never two valves open (`status`) | |
| `sim 2 0` then `water 2 1` | RUN FAULT (no flow) after ~30 s, zone 2 locked; `clear 2` unlocks | |
| `sim 3 15000` then `water 3 5` | RUN FAULT (burst) within ~15 s | |
| `run 4 30` | timed run stops at 30 s | |
| `stop` during a run | valve closes at once | |
| `time <epoch>` then `prog 1 1 HH:MM 1` for two minutes ahead | program fires once at that minute | |
| `auto off`, repeat the program | skipped | |
| press EN during a run | board resets, no valve output comes back on | |

Zone/sensor numbers in the console start at 1. `help` lists everything.

## Step 2 — node ⇄ gateway over LoRa (two boards, two E32)

1. `cp include/secrets.example.h include/secrets.h`, set `LINK_KEY` (same for
   both boards), WiFi and MQTT. Rebuild **both** firmwares.
2. Server: follow [../server/README.md](../server/README.md) (or just Mosquitto).
3. Flash `node-sim` (or `node`) and `gateway`.

| Check | Expected | OK? |
|---|---|---|
| `mosquitto_sub -v -t 'garden/#'` | telemetry every 60 s, status retained, gateway status | |
| Node boot | `boot` event; within ~30 s node prints `clock set` | |
| `{"cmd":"water","zone":1,"litres":2,"id":"t1"}` to `garden/node1/cmd` | `cmd/result` ok with id t1; run_started / run_finished events | |
| Same command with zone 9 | result invalid | |
| Command to a locked zone | result rejected, detail locked | |
| Gateway built with a different `LINK_KEY` | node prints "bad MAC", result bad_auth, nothing waters | |
| Power-cycle the gateway, send a command | still accepted (counter resync) | |
| Node unplugged, send a command | result no_answer after the retries | |
| Home Assistant | device "Garden node1" appears with soil, zone and button entities | |
| Garden map `http://<pi>:8080` | beds show live values | |

## Step 3 — real sensors and valves (after Phase 1)

Flash `node`. `cal dry` / `cal wet` for the moisture sensors, `k <sensor> <pulses/L>`
with the value measured in the flow test, then repeat the watering checks of
step 1 with real water, including closing the tap mid-run (no-flow fault) and
opening a line end (burst fault). Tune `ZONE_*` limits in `include/config.h`
to the measured zone flow.

## Known limitations (deliberate, to do next)

- **No RTC yet**: after a power cut, schedules wait until the gateway has set the
  clock. DS3231 support is the first Phase 3 item.
- Rain-skip is kept in RAM and lost on reboot.
- Time zone is a fixed offset in `secrets.h`; no automatic daylight saving.
- Radio collisions (node telemetry vs. a command) are only handled by retrying.
  `LINK_RETRY_MS` must be set from the air rate chosen in the range test.
- Zone settings are compile-time constants; the NVS/JSON settings module, the
  LCD/knob UI, service WiFi, HOA switch inputs, INA219 and local data log with
  backfill are not written yet.
- Soil probes are identified by bus order, not by address.
- Valves are driven from GPIOs; the I2C-expander backend comes with the IO board.
- Whether the YF-S201 can measure drip flow is still open (Phase 1, test 4). If
  not, zones fall back to timed watering until a low-flow sensor is fitted.

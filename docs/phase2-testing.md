# Phase 2/3 software — what exists and how to prove it on hardware

Everything here was written before any hardware was available. Verified so far:
all firmwares compile, and the pure logic passes 58 unit tests on the PC
(`pio test -e native`): frame codec, message codecs, command authentication,
irrigation core, scheduler, clock maths, data log. The server side passes a smoke
test against a fake node (`server/tests/smoke_test.py`). **Nothing has run on an
ESP32 yet**: no radio link, no real gateway, no flash filesystem, and the full
docker stack has never been started.

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

## Step 4 — DS3231 clock (when the module arrives)

Wire it to I2C (SDA 21, SCL 22, 3.3 V), coin cell fitted, flash `node`.

| Check | Expected | Result |
|---|---|---|
| Boot with a new chip | `RTC: found but time not set` | |
| `time <utc_epoch> <tz_minutes>` on the console | `clock set from console … RTC updated` | |
| `status` | correct local date/time, `RTC ok, box <temp> C` | |
| Unplug power for a minute, plug back in | `RTC: clock restored`, time and timezone correct | |
| Same with the gateway off | a program set with `prog` still starts on time | |
| Gateway time sync | `clock set from gateway … RTC updated` | |
| After a week | drift under a few seconds (compare with `status`) | |

## Step 5 — data log and backfill (two boards, gateway + broker running)

The node stores telemetry and events on its internal flash and uploads them when
the gateway acknowledges (PLAN section 12.1). The first boot formats the
filesystem and takes a few seconds longer. Works with `node-sim` too.

| Check | Expected | OK? |
|---|---|---|
| First boot | `log: 0 records stored`, no reset loop | |
| `status` after a few minutes | `log: 0 waiting to upload`, stored count rising | |
| `log dump 5` | CSV lines `seq,T,<utc>,…` and `seq,E,…` with plausible values | |
| Unplug the gateway for 15 min, water a zone meanwhile | node prints `no acknowledgement, keeping data in the log` once; `status` shows the backlog growing | |
| Plug the gateway back in | `link: back, N records to upload`; backlog falls to 0, about one record every 3–4 s | |
| `mosquitto_sub -v -t 'garden/#'` during catch-up | old samples and the watering events arrive on `…/backfill` topics with their original `t` | |
| Garden map / Home Assistant during catch-up | current values are not replaced by old ones | |
| Gateway running but broker stopped for 10 min | same behaviour: nothing lost | |
| Reset the node during catch-up | resumes; at most a few records repeated, none missing | |
| Send a `water` command during catch-up | accepted within a few seconds | |
| `server/data/archive/…jsonl` afterwards, then `tools/export_dataset.py` | no gap in `telemetry.csv` across the outage | |
| Node with no clock (no RTC, gateway off) | telemetry not logged (`status`: stored count only rises with events) | |
| After a week | `status`: flash use steady, `lost 0`; `log` shows no write errors | |

## Known limitations (deliberate, to do next)

- **Clock**: DS3231 support is written but untested (see Step 4). Without the
  chip, schedules wait after a power cut until the gateway has set the clock.
- Rain-skip is kept in RAM and lost on reboot.
- Time zone is a fixed offset in `secrets.h`; no automatic daylight saving.
- Radio collisions (node telemetry vs. a command) are only handled by retrying.
  `LINK_RETRY_MS` must be set from the air rate chosen in the range test.
- Zone settings are compile-time constants; the NVS/JSON settings module, the
  LCD/knob UI, service WiFi, HOA switch inputs and INA219 are not written yet.
- Data log: while catching up after an outage, telemetry on the dashboards lags
  until the queue is empty (status stays live). No SD card support yet.
- The weather forecast used for decisions is not archived yet.
- Soil probes are identified by bus order, not by address.
- Valves are driven from GPIOs; the I2C-expander backend comes with the IO board.
- Whether the YF-S201 can measure drip flow is still open (Phase 1, test 4). If
  not, zones fall back to timed watering until a low-flow sensor is fitted.

# Automated Garden — Master Plan (v0.2)

Status: architecture agreed. Phase 1 (bench bring-up) is waiting for hardware, see
[phase1-bringup.md](phase1-bringup.md). The Phase 2/3 software (protocol, irrigation
core, node app with simulator, gateway, server stack) is written and unit-tested on
the PC but **not yet run on hardware**, see [phase2-testing.md](phase2-testing.md). Items marked **[VERIFY]** are facts that must
be checked against the actual part / datasheet / site before we commit.

## 0. Decisions taken (2026-11-16)

| Topic | Decision |
|---|---|
| Water source | Mains tap. Pressure is not a constraint; a pressure reducer + filter for the drip lines is still needed |
| Zones | **6 zones = 3 beds × 2 half-bed zones**, on a **6-relay board**. Halves are watered the same for now; the split is there for later. One flow sensor per bed (it measures both halves, since zones run one at a time). No master valve for now |
| Parts on hand | YF-S201 flow sensors, 1/2" 12 V solenoid valves, capacitive v1.2 moisture, DS18B20 soil probes, DHT11 air. Used for the prototype; any of them may be replaced by better parts where Phase 1 shows a weakness |
| Radio | E32 433 MHz confirmed OK to use; antennas on hand; 50–60 m with trees + house walls |
| Server | Home Assistant + Mosquitto + InfluxDB/Grafana + our own `garden-brain` on the Pi 4 |
| Control box location | Mostly in sun (≈9:00–16:00) → sunlight-readable monochrome LCD, light-coloured/shaded enclosure, thermal margin |
| PCB | Hand-soldered carrier board for devkit modules, **stackable/modular** (section 4.9) |
| Ethernet | Future option; header reserved |
| Timeline | Working prototype in ~1 month; full system open-ended; budget available for extra parts |

---

## 1. Guiding principles

1. **The garden never depends on the network.** The field node waters correctly on its own
   (schedules + soil thresholds + safety) with LoRa, gateway and server all dead.
   The server only makes it *smarter*, never makes it *work*.
2. **Fail dry, not flooded.** Every failure mode (MCU crash, lost packet, brown-out, stuck
   firmware) must end with valves closed. This drives several hardware choices below.
3. **Commands are intents, not switch states.** The server says "give zone 1 four litres"
   (with an expiry), never "open valve" followed later by "close valve". A lost packet
   can then never leave water running.
4. **MQTT is the contract.** Everything server-side talks MQTT + JSON. The IoT platform
   (Home Assistant, ThingsBoard, our own) becomes a replaceable consumer.
5. **Transport is pluggable.** Firmware talks to a `Link` interface. LoRa today; Ethernet
   or WiFi later without touching the application logic.
6. **Same firmware style as `firmware-example/`**: one `lib/` module per concern,
   `main.cpp` only wires modules, one test firmware per module, a simulator mode,
   serial console, settings in NVS as JSON, unit tests.

---

## 2. System architecture

```
 GARDEN (no WiFi)                         HOUSE (WiFi/LAN)
┌──────────────────────────────┐        ┌───────────────────┐      ┌──────────────────────────────┐
│ FIELD NODE  (ESP32 + E32)    │  LoRa  │ GATEWAY           │ WiFi │ SERVER (Raspberry Pi 4 8GB)  │
│                              │ 433MHz │ (ESP32 + E32)     │ MQTT │ docker compose:              │
│ sensors ─► irrigation core ──┼────────┼─► LoRa⇄MQTT bridge├──────┼─► Mosquitto (broker)         │
│ valves ◄── safety layer      │        │   NTP time source │      │   Home Assistant (UI/phone)  │
│ flow   ─► dosing / leak det. │        │   HA discovery    │      │   InfluxDB + Grafana (history)│
│ LCD + knob + HOA switches    │        └───────────────────┘      │   garden-brain (our Python)  │
│ RTC, local log (flash)       │                                   │   Tailscale (remote access)  │
│ service WiFi AP (OTA/config) │                                   └──────────────────────────────┘
└──────────────────────────────┘
```

### Control hierarchy (who is allowed to decide what)

| Level | Where | What it does | Works without |
|---|---|---|---|
| L0 | Hardware HAND-OFF-AUTO switch per valve | Forces a valve on/off electrically | MCU, everything |
| L1 | Node safety layer | Max-on time, max litres, leak/burst/no-flow detection, one zone at a time | Link, server |
| L2 | Node irrigation core | Schedules, soil-moisture thresholds, manual runs from the on-site menu | Link, server |
| L3 | Server (`garden-brain`) | Adjusts budgets/thresholds, skips on rain forecast, sends dosing intents | — |
| L4 | You (phone/PC) | Dashboards, manual commands, approve/override recommendations | — |

Lower levels always win. L1 can veto anything from above.

---

## 3. Key decisions and my recommendation

### 3.1 Field controller: **ESP32, not Arduino Mega**

| | ESP32 (you have 2) | Mega |
|---|---|---|
| Logic level | 3.3 V, matches the E32 directly | 5 V, needs shifting for E32 |
| RAM/flash | 520 KB / 4 MB, LittleFS for local logging | 8 KB / 256 KB |
| RTOS / dual core | yes, same task model as your blinds firmware | no |
| On-site WiFi AP for phone config + OTA | yes | no |
| Crypto (HMAC for commands) | hardware-accelerated | painful |
| GPIO count | tight, solved with I2C parts | plenty |
| ADC quality | poor | ok |

The two ESP32 weaknesses (pins, ADC) are both solved by putting analog on I2C ADCs,
which we want anyway for 10 moisture channels. One ESP32 NodeMCU becomes the node, the
other becomes the gateway. Pi Pico and Pi Zero W stay spare (Zero W is the natural
camera board later).

### 3.2 Radio: E32-433T20D as a point-to-point link

What these modules are and are not **[VERIFY against datasheet]**:
- SX1278 LoRa radio behind a UART with EBYTE's own framing. **Not LoRaWAN**, so no
  ChirpStack/TTN. We design our own small protocol on top (section 7).
- 3.3 V logic. Supply 3.3–5.2 V; 5 V supply recommended for full 20 dBm output.
- Pins: TX, RX, M0, M1 (mode), AUX (busy). Configured in mode 3 at 9600 8N1.
- About 58 bytes per packet in fixed-transmission mode: every message we define must
  fit in one packet.
- **No encryption or authentication.** We add a truncated HMAC + rolling counter on
  anything that actuates. Telemetry can stay plain.
- TX current peaks above 100 mA: bulk capacitor right at the module.

**Biggest project risk = range.** 433 MHz at 20 dBm with the stock stub antenna through
walls/trees may or may not reach. A range test at the real site is step one of Phase 1,
before any other hardware work. Budget for two proper 433 MHz antennas (SMA, mounted
high, outside the enclosure).

**[VERIFY] Regulation:** 433 MHz is an ISM band in Europe/Africa but not everywhere.
Check the allowed power/duty cycle where you live before running 20 dBm continuously.
Our duty cycle will be tiny (a 40-byte packet every minute or so) which helps.

### 3.3 Gateway: second ESP32 + second E32, bridging to MQTT

Alternative considered: plug the E32 into the Pi 4 through a USB-UART adapter. Fewer
parts, but it chains the Pi's physical location to the antenna's best spot. A separate
ESP32 gateway can sit at the window/roof nearest the garden with only a USB charger,
and the Pi lives wherever. The gateway stays deliberately dumb: binary LoRa frames in,
JSON MQTT out, and the reverse.

### 3.4 IoT server: **Home Assistant + Mosquitto + InfluxDB/Grafana + our own small service** — not ThingsBoard, not fully custom

| Option | For | Against |
|---|---|---|
| ThingsBoard CE | Device-centric, nice dashboards, RPC, rule chains | Heavy (Java + Postgres), rule chains get clumsy for real logic, weak camera story, mobile app/notifications less polished, some features PE-only |
| Fully custom (own backend + web app) | Total control | Months of work re-building auth, dashboards, phone app, notifications. Not where the value is |
| **Home Assistant + friends** | Free phone app with push notifications; MQTT discovery (firmware declares its entities, UI appears by itself); cameras, weather stations and forecasts are first-class integrations; huge community; easy remote access | Long-term history is mediocre on its own (hence InfluxDB/Grafana); it is "home" flavoured |

What we *do* write ourselves is only the part that is unique to this project:
**`garden-brain`**, a small Python service on the Pi that
- subscribes to telemetry, stores derived data,
- computes water demand (reference evapotranspiration from weather + crop factors, soil
  moisture trend, rain forecast from Open-Meteo: free, no key, you already used it),
- emits recommendations or dosing intents over MQTT,
- detects anomalies (sensor drift, slow leak, clogged emitters: flow per zone falling
  over weeks),
- optionally calls an LLM (Claude API) for a daily plain-language summary and advice
  from the logged data and, later, camera images.

Because of principle 4, if you later prefer ThingsBoard it can be added next to HA as
another MQTT consumer without changing firmware.

Pi 4 notes: boot from SSD, not SD card (databases kill SD cards). Remote access with
Tailscale (free, no port forwarding). Nightly backup of HA + Influx volumes.

### 3.5 Ethernet (your 50 m cable idea)

50 m is well inside the 100 m limit. What it would buy: fast reliable link, real OTA,
**cameras** (impossible over LoRa), and power over the same cable (PoE → 12 V).
What it costs: trenching/outdoor-rated cable, and a long outdoor copper run is a
surge/lightning path into the house network (needs surge protectors at both ends).

Recommendation: **build v1 on LoRa as specified**, keep the `Link` abstraction, and
leave a header on the PCB for a W5500 SPI Ethernet module. Decide on the cable when
cameras become a real goal; at that point a pair of cheap outdoor point-to-point WiFi
bridges is the no-trench alternative.

---

## 4. Field node hardware design

### 4.1 Block diagram

```
 12V in ─ fuse ─ reverse-polarity FET ─ TVS ─┬─► VALVE RAIL ─ watchdog-gated switch ─► 4x relay ─► solenoids (flyback diode on each coil)
 (adapter now, battery+MPPT later, 9–16 V)   ├─► buck 5 V ─► flow sensors, E32, LCD backlight
                                             └─► 3.3 V   ─► ESP32, I2C devices, moisture sensors (switched)

 ESP32 ── UART2 + M0/M1/AUX ─────────► E32-433T20D ─► SMA ─► external antenna
       ── I2C ─► 3x ADS1115 (12 analog ch) ◄─ 10x capacitive moisture (+2 spare)
              ─► DS3231 RTC (battery backed)
              ─► SHT3x/SHT4x or BME280 (air T/RH[/pressure])
              ─► INA219 (supply voltage/current: valve current check + battery monitor)
              ─► (opt.) VEML7700/BH1750 light, MCP23017 I/O expander
       ── 1-Wire ─► 2x DS18B20 soil temperature (more can share the same pin)
       ── 3x pulse inputs (PCNT) ◄─ flow sensors (5 V → 3.3 V level shifted, RC filtered)
       ── SPI/I2C ─► display      ── 3 GPIO ◄─ EC11 knob
       ── HOA switch sense inputs (via expander)
       ── RS-485 transceiver (expansion bus: remote bed modules, Modbus weather sensors)
       ── header: W5500 Ethernet (future)
```

### 4.2 Things you own, and what I'd watch for

| Part | Notes / risks |
|---|---|
| **Capacitive moisture v1.2 ×10** | Cheap batches are often defective (missing 3.3 V regulator, wrong 555 variant, missing 1 MΩ resistor → slow/garbage readings). Test all 10 first. Electronics end and PCB edges must be sealed (conformal coat/epoxy + heat-shrink) or they die in weeks. Output is relative: calibrate each one dry/wet, and store per-sensor calibration. Power them from a switched rail and read a few times an hour: less corrosion, less power. 4–5 per bed lets us average and detect an outlier. |
| **Flow sensors ×3** | YF-S201 (1–30 L/min). A single drip bed draws roughly 0.5–1.5 L/min (my estimate: ~30 emitters × 2 L/h ≈ 1 L/min), which is at or below the sensor's minimum: it may read badly or not spin. A YF-S401/S402-class sensor (≈0.3–6 L/min) fits drip far better. We measure real flow in Phase 1 and decide. |
| **12 V solenoid valves ×3** | **[VERIFY]** normally-closed (must be) and their minimum pressure: common plastic ones are pilot-operated and need ≈0.2 bar or more, so they will not open on a gravity tank. They draw ~0.3–0.5 A and run hot when held: fine on mains, expensive on solar (latching valves later; the driver design will leave room for an H-bridge option). |
| **Soil temp ×2, air T/RH ×2** | DS18B20 probes are fine long-term. The DHT11 is the weakest part you own (±2 °C, ±5 %RH, 20–90 %RH, poor outdoors): OK for the prototype, replace with an SHT31/SHT40 in a radiation shield (a few dollars) for the real install. |

### 4.3 Zones and plumbing

```
 source ─ shutoff ─ filter (120 mesh) ─ [MASTER valve + flow] ─ pressure reducer (~1–1.5 bar) ─┬─ [valve A + flow] ─ bed 1 drip
                                                                                              └─ [valve B + flow] ─ bed 2 drip
```

**Decided: 6 zones (3 beds × 2 halves), relay-switched, mains water.** Each bed
has one flow sensor feeding its two zone valves:

```
 reducer ─┬─ flow 1 ─┬─ valve 1 ─ bed 1 left      (same for beds 2 and 3:
          │          └─ valve 2 ─ bed 1 right      flow 2 → valves 3/4, flow 3 → valves 5/6)
```

(The first diagram shows the master-valve variant; without a master the bed
branches hang directly off the pressure reducer.) Trade-off accepted: with no
master valve, a zone valve that sticks open can only be detected (flow with all
valves commanded closed → alarm, and the firmware names the bed), not stopped. A
master valve needs a 7th output; the firmware already supports one (`masterValve`).

Relays (rather than MOSFETs) give isolation and can later switch 24 VAC irrigation
valves or a pump. Consequences for the design: a flyback diode across every solenoid
coil (otherwise contact arcing resets the ESP32), relay outputs that stay off during
boot, and the hardware watchdog still gating the valve supply.

Flow sensing option to evaluate in Phase 1: since zones run one at a time, **one
flow sensor on the common inlet** measures every zone and also sees leaks, instead
of one sensor per zone. With mains pressure the inlet sees the same low drip flow,
so the low-flow accuracy question is the same either way.

Filter and pressure reducer are not optional for drip, even more so on mains
pressure: emitters clog and fittings pop.

### 4.4 Safety hardware (cheap, and what makes it trustworthy)

- **Gate pull-downs** on every valve MOSFET: valves off during boot/reset/flashing.
  No valve on an ESP32 strapping pin (your blinds README already has that scar).
- **Hardware watchdog on the valve rail**: the 12 V valve supply is only enabled while
  the firmware toggles a heartbeat pin (AC-coupled charge pump or retriggerable
  monostable). MCU frozen with a valve open → rail drops in about a second.
- **HAND-OFF-AUTO switch per valve** with the position readable by the MCU. HAND works
  with a dead MCU (your "no IoT" requirement at its most basic), OFF is a hard lockout
  for maintenance, AUTO is normal. Firmware alarms if a switch is left in HAND.
- Flyback diodes, a fuse on the valve rail, INA219 to confirm the valve really draws
  current when commanded (detects a broken wire or dead coil).
- TVS/series resistors/RC on every line that leaves the enclosure.

### 4.5 On-site UI

- **Display (decided: sunlight-readable mono):** the box sits in sun most of the day,
  and TFTs wash out in sunlight. Use a **128×64 monochrome graphic LCD (ST7920)**:
  readable in sun, wide temperature range, cheap, and graphic so the menu system can
  keep the feel of your knob UI. Mount it behind a small visor/window.
- EC11 knob + click (same `rotary_encoder` module as the blinds), plus the HOA
  switches and one status LED per valve.
- Menu: status home (zones, last/next watering, moisture, link), manual run (zone,
  minutes or litres), schedules, thresholds, sensor calibration, diagnostics, link
  test, "service WiFi" toggle.
- **Service WiFi**: from the menu the node raises its own AP for a few minutes; phone
  connects on-site for the full web UI, logs and firmware upload. This is how OTA
  works without WiFi coverage (OTA over LoRa at ~2.4 kbps is not realistic).

### 4.6 Power, sized for solar from day one

- Input 9–16 V so the same board runs from a 12 V adapter now and a 12 V LiFePO4 + MPPT
  controller later. Battery voltage/current already measured by the INA219.
- Rough idle estimate: ESP32 (WiFi off) + E32 in receive + LCD ≈ 0.5–1 W →
  12–24 Wh/day. That points to roughly a 20–30 W panel and a 12 V 7–12 Ah battery,
  to be re-done with measured numbers and your sun hours.
- Firmware power features (sensor rail switching, backlight timeout, light sleep
  between samples) are designed in but tuned later.

### 4.7 Enclosure and wiring

IP65+ box, cable glands downward, pressure vent plug, pluggable screw terminals,
labelled per-channel, conformal coating on the PCB, antenna outside on an SMA
bulkhead, air sensor outside in a radiation shield. Valve/plumbing manifold in a
separate box below the electronics.

### 4.8 PCB approach

- KiCad. **Carrier board**: the ESP32 devkit and the E32 plug into sockets; everything
  else (power, drivers, ADCs, RTC, protection, connectors) is on our board. Fast, easy
  to repair, and you already own the modules. A bare-module (ESP32-WROOM soldered)
  revision only makes sense after the design is field-proven.
- 2 layers, through-hole + large SMD, hand-soldered (decided).
- Order: breadboard/perfboard prototype for bring-up → schematic → layout → DRC/DFM →
  order v1. The `kicad-pro`/`kicad-review` tools in this workspace are available for
  ERC/DRC/DFM checks when we get there (a third `kicad` server is configured but
  failed to connect this session).

### 4.9 Stackable board system (requirement)

The controller is a **stack of boards on a common bus connector**, not one big PCB,
so future functions are added by plugging in a module instead of redesigning.

```
        ┌────────────────────────────┐
        │ future module (camera/Eth, │   each module: same outline, same
        │ extra valves, weather, …)  │   stacking header position, 4 corner
        ├────────────────────────────┤   standoffs, ID EEPROM
        │ IO module (relays, flow,   │
        │ sensor terminals, ADCs)    │
        ├────────────────────────────┤
        │ CORE (ESP32 devkit, E32,   │
        │ power, RTC, watchdog, UI)  │
        └────────────────────────────┘
```

- **Stacking bus** (2×20 stackable pin header, exact pinout fixed in Phase 4):
  power rails (12 V in, gated 12 V valve rail, 5 V, 3.3 V, GND), I2C, SPI + a few
  chip-selects, one UART, RS-485 A/B, a shared interrupt line, a handful of
  uncommitted GPIOs, and the watchdog/"outputs enable" line so every module's
  actuators obey the same fail-safe.
- **Prefer bus devices over raw GPIO** on modules (I2C expanders/ADCs with address
  jumpers), so adding a module does not consume core pins and two identical modules
  can coexist.
- **Module identity**: a small I2C EEPROM on each board (type, revision, address
  config). The firmware scans at boot and enables the matching `lib/` module, which
  fits the module-per-concern firmware layout.
- **Split for v1**: *Core* + *IO-1* (4 relays, 4 flow inputs, 12 moisture channels,
  1-Wire, sensor terminals). Splitting these two from the start proves the bus on
  day one. Candidates later: IO-2 (more zones), Ethernet/PoE, camera carrier
  (Pi Zero, powered and supervised by the core), weather/RS-485, solar/charger monitor.
- Mechanical: fixed board outline and hole pattern, field connectors only on board
  edges so they stay reachable in a stack, enclosure sized for 4–5 boards.

### 4.10 Pin budget, and what happens when pins run out

The ESP32 devkit has about 15 freely usable GPIOs plus 4 input-only ones. The
breadboard prototype drives everything directly and is already close to full:

| Use | GPIOs | Final board |
|---|---|---|
| LoRa UART + mode + AUX | 16, 17, 25, 27 | same |
| I2C | 21, 22 | same (everything slow lives here) |
| 1-Wire | 4 | same |
| Flow pulses ×3 (4th possible on 39) | 34, 35, 36 | same: input-only pins, need real interrupts |
| DHT11 | 14 | freed (SHT3x/4x is on I2C) |
| 6 relays | 18, 19, 23, 26, 32, 13 | **moved to an I2C expander**, frees all six |
| Still to place | | display (SPI 18/23 + CS), knob (3), watchdog heartbeat, sensor-rail switch, RS-485 (3), W5500 (MISO 19, CS, INT) |

So the answer to "what if we run out": **nothing slow gets a raw GPIO on the final
boards.**

1. **MCP23017 I2C expander (16 I/O) on the IO module** for relays, HOA switch
   sensing and LEDs. Its pins power up as inputs, so with a pull resistor on each
   relay driver the valves stay off through reset and flashing, and the
   watchdog-gated valve rail still cuts power if the MCU hangs. Each expander has
   3 address jumpers: up to 8 per bus = 128 I/O on two wires. A second IO module
   costs zero core pins.
2. **ADS1115** for every analog input (already planned): 4 per bus = 16 channels.
3. **Only timing-critical signals keep GPIOs**: flow pulses, radio UART, knob,
   SPI. The knob can also move to the expander's interrupt if needed.
4. Alternative for outputs only: a 74HC595 shift register with its output-enable
   pin tied to the watchdog (3 GPIOs for any number of outputs).
5. Beyond one enclosure: RS-485 bed modules, each with its own small MCU.

Firmware impact is small: `ValveBank` gets a pin-driver backend (GPIO now,
MCP23017 later); nothing above it changes.

### 4.11 Expansion strategy

- More sensors near the box: free ADS1115 channels, more DS18B20 on the same wire, I2C.
- More valves/inputs: another IO module on the stack (4.9).
- More beds further away: **RS-485 bus** (12 V + A/B in one cable) with small "bed
  modules" (cheap MCU + moisture inputs + 1–2 valve drivers). Same bus speaks Modbus
  to off-the-shelf industrial soil probes and weather sensors.
- More sites: additional field nodes on the same LoRa channel; protocol has node
  addresses from the start.

---

## 5. Future items and how v1 prepares for them

| Future item | Preparation in v1 |
|---|---|
| Weather station | Three paths, all compatible: (a) WiFi station with local API (e.g. Ecowitt) straight into Home Assistant; (b) a 433 MHz consumer station decoded by an RTL-SDR dongle + `rtl_433` on the Pi; (c) DIY/Modbus sensors on the node's RS-485 port. Until then, Open-Meteo forecast + our own air sensor. |
| Solar | 9–16 V input, power monitoring, low-power firmware hooks, latching-valve driver option. |
| Cameras | Needs IP connectivity at the garden (Ethernet or WiFi bridge). Pi Zero W + camera does stills/timelapse; HA shows the stream. Not possible over LoRa. |
| More capacity | RS-485 bed modules, node addressing, zones as data not code. |
| Fertigation, tank level, pump, greenhouse vent/fan (your steppers) | Spare outputs/inputs and the modular firmware HAL. |

---

## 6. Firmware architecture (PlatformIO, mirrors `firmware-example/`)

One PlatformIO project, two applications sharing libraries:

```
firmware/
├── platformio.ini        envs: node, node-sim, gateway, test (TEST=<name>), unit (on-device), native (host unit tests)
├── include/config_node.h, config_gateway.h     pins and hardware constants
├── src/node/main.cpp     wires modules together
├── src/gateway/main.cpp
├── src/tests/            one bring-up firmware per module
├── test/                 unit tests (protocol codec, irrigation logic, scheduler, calibration math)
├── lib/
│   ├── protocol/         message structs, encode/decode, CRC, HMAC      (shared, no Arduino deps → host-testable)
│   ├── lora_link/        E32 driver (modes, config, AUX), Link interface (shared)
│   ├── link_manager/     ack/retry, sequence numbers, store-and-forward queue
│   ├── valves/           drivers, watchdog heartbeat, HOA sense, current check, SimValves
│   ├── flow/             pulse counting (PCNT), calibration, litres, SimFlow
│   ├── soil/             ADS1115 moisture + calibration, DS18B20, SimSoil
│   ├── climate/          air T/RH, light
│   ├── power/            INA219, sensor-rail switching, sleep policy
│   ├── timekeeping/      DS3231, time sync from gateway
│   ├── irrigation/       zone state machines, dosing by time/volume, safety interlocks, fault handling
│   ├── scheduler/        programs (time/weekday/sunrise offset), soil-threshold rules, rain-skip flag from server
│   ├── datalog/          ring log on LittleFS, backfill after link outage
│   ├── telemetry/        builds periodic and event messages
│   ├── commands/         validates + executes remote intents
│   ├── settings/         NVS + JSON (as in the blinds firmware)
│   ├── display/, ui/     screens + menus, rotary_encoder reused
│   ├── service_wifi/     on-demand AP, web UI, OTA
│   ├── console/, app_log/   serial console and ring-buffer logger (as in the blinds firmware)
│   └── gateway_bridge/   LoRa⇄MQTT mapping, HA MQTT discovery, NTP, WiFi portal, OTA
└── scripts/, tools/      test.sh, select_test.py, link range logger, sensor calibration helper
```

Carried over from your example: module-per-lib, simulated hardware implementations,
`node-sim` running the whole firmware on a bare ESP32, test firmware per module.
Added: a `native` env so pure logic (protocol, irrigation state machine, scheduler)
is unit-tested on the PC in seconds, since those are the parts where a bug floods a bed.

Task layout (node): irrigation/safety task (fast, highest priority) · sensor task ·
link task · UI loop · service-WiFi task only when enabled.

Irrigation zone state machine: `IDLE → (master open) → PRIMING (expect flow within N s)
→ DOSING (until litres or time) → CLOSING (expect flow to stop) → IDLE`, with exits to
`FAULT_NO_FLOW`, `FAULT_OVERFLOW`, `FAULT_LEAK`, `FAULT_VALVE_CURRENT`, each of which
closes everything, logs, raises an event and (configurably) locks the zone until cleared.

---

## 7. Communication design

### LoRa protocol (binary, implemented in `firmware/lib/protocol`)

`[SYNC][LEN][dst][src][type][seq][payload ≤ 48][CRC16]`, payloads little-endian.
Gateway → node messages carry a rolling 32-bit counter and end with a 4-byte
truncated HMAC-SHA-256 (shared key in `include/secrets.h`) over header + payload.
The node accepts only a counter above the last accepted one (persisted), so a
recorded command cannot be replayed; commands also carry an expiry time.
TELEMETRY, STATUS, EVENT, CMD, TIME and ACK are implemented; BACKFILL and CONFIG
are reserved.

| Type | Direction | Content |
|---|---|---|
| TELEMETRY | node → gw | compact sensor snapshot, every 1–5 min (config) |
| STATUS | node → gw | zone states, faults, uptime, supply V, RSSI, on change + heartbeat |
| EVENT | node → gw | watering started/finished (litres, duration), faults, HOA changes; acked + retried |
| BACKFILL | node → gw | logged records missed during an outage, sent slowly when idle |
| CMD | gw → node | intents: water zone X litres/minutes (with expiry), stop all, skip next, set mode; authenticated, acked |
| CONFIG | both | get/set a settings key (schedules, thresholds), chunked |
| TIME | gw → node | epoch sync |
| ACK/NACK | both | by seq |

The node listens continuously in v1 (mains power), so commands arrive within seconds.
For solar we can move to wake-on-radio or "node polls after each uplink".

### MQTT topics (JSON)

```
garden/<node>/telemetry      garden/<node>/status       garden/<node>/event
garden/<node>/cmd            garden/<node>/cmd/result    garden/<node>/config/…
garden/gateway/status        garden/brain/recommendation
homeassistant/…/config       (auto-discovery, published by the gateway)
```

---

## 8. Data-driven features (server side, incremental)

1. **Logging + dashboards**: moisture per sensor/bed, soil and air temperature, RH,
   litres per zone per day, valve runs, link quality, supply voltage.
2. **Alerts** to the phone: fault events, no telemetry for N minutes, moisture below
   critical, frost/heat warnings, switch left in HAND.
3. **Rain/forecast-aware watering**: skip or reduce before forecast rain; add before heat.
4. **Water-budget model**: evapotranspiration × crop coefficient per bed, corrected by
   the measured moisture response (how much does 1 L move the reading, how fast does
   it dry). Starts as *recommendations* you approve, becomes automatic once trusted.
5. **Health analytics**: sensor drift/outlier detection, emitter clogging (same valve
   time → fewer litres), slow leaks.
6. **AI layer**: daily/weekly digest and advice from the data; later camera images
   for growth tracking and problem spotting.
7. **Garden map and planting journal**: a visual layout of the beds showing what is
   planted where, with live data on top. `server/garden.yaml` is the single
   description of the garden (beds, zones, sensor positions, plantings, crop
   coefficients); `garden-brain` uses it to tag all logged data by bed/zone and
   serves the map. A first version exists (zones coloured by moisture, sensors,
   crops, litres today, zone state/faults, recent events). Later: planting history
   and harvest log per bed, editing plantings from the page, per-bed charts and
   season comparisons, growth-stage-aware water budget, camera images per bed,
   embedded as a Home Assistant panel.

---

## 9. Roadmap

| Phase | Goal | Main deliverables |
|---|---|---|
| **0. Foundations** | Agree on this plan | Answers to section 11, repo scaffold (`docs/ firmware/ hardware/ server/`), git, shopping list ordered |
| **1. Bench bring-up + risk tests** | Kill the big unknowns | **LoRa range test at the real site**; test firmware per module; characterise all 10 moisture sensors; flow sensor accuracy at real drip flow; valve current/pressure check |
| **2. End-to-end data path** | Sensor reading visible on the phone | Protocol lib + unit tests, gateway firmware, Pi stack (docker compose), HA dashboard, node sending real telemetry from a breadboard |
| **3. Irrigation core** | Safe autonomous watering on a prototype | Zone state machine, safety layer, schedules, manual UI on LCD/knob, remote intents, local log + backfill; field trial on one bed with the perfboard prototype |
| **4. PCB + enclosure** | Robust permanent install | KiCad schematic/layout, review, order, assemble, enclosure, plumbing manifold, install both beds |
| **5. Smart layer** | Data-driven decisions | `garden-brain`: forecast skip, water budget, anomaly detection, recommendations, AI digest, full garden map + planting journal |
| **6. Extensions** | As wanted | Solar, weather station, camera, RS-485 bed modules, more zones |

Phases 2 and 3 firmware can largely be developed in simulator mode while parts ship.

---

## 10. Probable shopping list (after your answers)

- 6-channel 5 V opto-isolated relay board, 1N4007 diodes for the valve coils, 3 more valves (6 total)
- MCP23017 I2C expander module (final IO board; useful to prototype early)
- 3× ADS1115 modules, 1× DS3231 RTC, 1× SHT31/SHT40 (+ radiation shield), 1× INA219
- Buck converter 12 V→5 V, fuses, TVS diodes
- Display (per 4.5), EC11 knob, 3-position switches, LEDs
- RS-485 transceiver (MAX3485 or similar), optional W5500 module
- Drip hardware: filter, pressure reducer, tubing, emitters, fittings
- Possibly: low-flow sensors (YF-S401/S402 class), 4th valve
- IP65 enclosures, cable glands, vent plug, pluggable terminal blocks, conformal coating
- SSD + USB adapter for the Pi 4
- 12 V outdoor-rated supply (≥2 A)

---

## 11. Open questions

Answered ones are recorded in section 0. Still open:

1. Exact drip layout per zone (emitter count and rating) → real flow per zone, which
   decides whether the YF-S201 is usable (Phase 1 flow test).
2. Enclosure size/model → board outline for the stack.
3. Whether tomatoes and lettuce get separate zones inside a box (3 zones for 2 boxes
   leaves one free).

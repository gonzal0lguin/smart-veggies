# Phase 1 — Bench bring-up and risk tests

Goal: retire the unknowns that could change the design, before any PCB work.
Each test is its own small firmware (same pattern as the blinds project):

```bash
cd firmware
./scripts/test.sh <lora_range|soil|flow|valves|climate> [serial-port]
./scripts/test.sh build-all        # compile everything, no board needed
pio test -e native                 # protocol unit tests on the PC
```

Suggested order: **1 → 2 → 3 → 4 → 5**. Test 1 is the one that can change the
architecture, so do it first. Fill in the result tables as you go; they decide
what gets bought and what goes on the PCB.

## Pin map (breadboard, ESP32 NodeMCU-32S) — `firmware/include/config.h`

| Function | GPIO | Notes |
|---|---|---|
| E32 TXD → ESP RX2 | 16 | |
| E32 RXD ← ESP TX2 | 17 | |
| E32 M0 **and** M1 (tied together) | 25 | low = normal, high = config |
| E32 AUX | 27 | low = busy |
| I2C SDA / SCL | 21 / 22 | ADS1115 and later RTC etc. |
| DS18B20 data (1-Wire) | 4 | 4.7 kΩ pull-up to 3.3 V |
| DHT11 data | 14 | bare sensor: 10 kΩ pull-up to 3.3 V |
| Flow sensors 1–3 (one per bed) | 34, 35, 36 | **through a 5 V → 3.3 V divider** |
| Valve relays 1–6 | 18, 19, 23, 26, 32, 13 | zones 1–2 = bed 1, 3–4 = bed 2, 5–6 = bed 3 |
| Moisture fallback (no ADS1115) | 39, 33 | two sensors at a time |

Power: E32 VCC from 5 V (3.3 V works at reduced output power), 100 µF or more
across its supply pins, antenna connected **before** powering it. Moisture sensors,
ADS1115 and DS18B20 from 3.3 V. Flow sensors from 5 V. All grounds common.

---

## Test 1 — LoRa range (`lora_range`) — do this first

The same firmware goes on both ESP32s, each with an E32.

1. On the bench, both boards on USB: check each prints `E32 found` and a config
   line (`chan=23 (433 MHz) air=2.4k power=20dBm`). If it prints "no answer", the
   wiring or power is wrong. Both must show identical settings.
2. `role pong` on one, `role ping` on the other. The ping side should print
   `OK rtt=… ms` every 2 s.
3. Put the **pong** board where the gateway would realistically live (window or
   wall facing the garden, as high as practical). Take the **ping** board on a
   power bank to the garden, at the height the control box will have.
   LED on the ping board: short blinks = OK, solid = lost.
4. Leave it 10 minutes per configuration, then read `stats` with a laptop
   (or the pong side's "missing" counter back in the house).

| Configuration | Sent | Lost % | Worst streak | RTT avg (ms) |
|---|---|---|---|---|
| rate 2 (2.4k, default), size 16 | | | | |
| rate 2, size 40 (real telemetry size) | | | | |
| rate 1 (1.2k), size 40, period 3000 | | | | |
| rate 0 (0.3k), size 40, period 6000 | | | | |
| best rate, antenna in final position, valves/relay switching nearby | | | | |

Pass: **under 2 % loss and no streak longer than 2** at some air rate. The protocol
will retry, so this gives a very reliable link. Lower air rate = more range but
longer packets. If no setting passes: move/raise the gateway antenna first, then
consider directional antennas or the Ethernet option.

Also try other spots for the gateway: the result tells us where it has to live.

## Test 2 — Moisture sensors (`soil`)

Screen all 10 before sealing or installing any.

For each sensor: stable in air, drops about 1 V in water, settles in about a
second. Then `dry` (all in air) and `wet` (all in water up to the line) and `cal`.

| # | Air (mV) | Water (mV) | Span (mV) | Noise (±mV) | Settles in | OK? |
|---|---|---|---|---|---|---|
| 1 | | | | | | |
| … | | | | | | |
| 10 | | | | | | |

Reject: span under ~500 mV, settling slower than ~3 s, noisy. Sensors with similar
spans can share a bed so their readings are comparable.

After screening: seal the electronics end (conformal coat or epoxy, then adhesive
heat-shrink) and the cut PCB edges with varnish, then re-check in water.

Second pass, with the final cable length (3–5 m) attached: the reading and noise
should not change noticeably. If they do, we add filtering or shorten runs.

## Test 3 — Valve relays (`valves`)

1. Relay board only. Watch it while plugging in USB, pressing EN/reset and while
   flashing: **no relay may click**. Typical 5 V opto-isolated boards are
   active-low and stay off with a 3.3 V "high" — but some do not. If a relay is
   on while the ESP32 is in reset, note it: the PCB will need a transistor stage
   and pull resistors on those lines.
2. `on 1 5`, `cycle`.
3. Wire a valve through the relay's COM/NO contacts to 12 V with a **1N4007
   across the coil** (stripe to +12 V). Check it opens/closes with water pressure
   and that the ESP32 does not reset or glitch when it switches.
4. Measure the valve's current and how hot it gets after 15 minutes open.

| Check | Result |
|---|---|
| Relay state during reset/flash | |
| Relay board logic (active low/high), works from 3.3 V? | |
| ESP32 stable when valve switches (with diode) | |
| Valve current at 12 V (mA) | |
| Valve temperature after 15 min | |
| Valve fully closes (no weeping) at mains pressure | |

## Test 4 — Flow sensors at drip flow (`flow`)

The YF-S201 is rated 1–30 L/min; one drip zone is around 1 L/min or less. This
test decides whether to keep it.

Signal wiring: sensor output → 10 kΩ → GPIO, and GPIO → 20 kΩ → GND.

1. Blow through each sensor: pulses count.
2. With a real drip line (or a tap throttled to the same trickle) into a
   measuring jug: `reset`, run 2+ litres, `cal 1 <litres>`. Three runs.
3. Repeat at about half and double that flow.
4. `dose 1 2.0` and measure what actually came out.

| Run | Real flow (L/min) | Litres (jug) | Pulses | Pulses/L |
|---|---|---|---|---|
| typical 1 | | | | |
| typical 2 | | | | |
| typical 3 | | | | |
| half flow | | | | |
| double flow | | | | |

Keep the YF-S201 if pulses/L agrees within about ±5 % across runs and flows and
it never stalls. Otherwise order a low-flow sensor (YF-S401/S402 class,
≈0.3–6 L/min) and re-run this test; the firmware only needs a new constant.

## Test 5 — Temperature / humidity / buses (`climate`)

- DS18B20: both probes found; note which address is which probe (warm one by hand).
- DHT11: plausible readings. (Known limits: ±2 °C, ±5 %RH; placeholder for an SHT3x/4x.)
- I2C scan lists the ADS1115s once they arrive.

| Probe address | Label / bed |
|---|---|
| | |
| | |

---

## To buy now (needed during or right after Phase 1)

| Item | Why |
|---|---|
| 3× ADS1115 module | 12 moisture channels (test 2 works without them, two sensors at a time) |
| 6-channel 5 V opto-isolated relay board, 1N4007 diodes, 3 more valves | 6 zones (3 beds × 2 halves) |
| MCP23017 I2C expander module | relays move onto it on the final board (PLAN 4.10) |
| Resistors: 4.7 kΩ, 10 kΩ, 20 kΩ; 100–470 µF capacitors | pull-ups, flow dividers, E32 supply |
| 12 V supply ≥ 2 A, small 12 V → 5 V buck module | valves + electronics |
| Drip kit: filter, pressure reducer, tubing, emitters | needed to measure real zone flow |
| Conformal coating / epoxy, adhesive heat-shrink | sealing moisture sensors |
| SHT31 or SHT40 breakout | replaces the DHT11 for the real install |
| DS3231 RTC, INA219 | Phase 3, cheap to add to the same order |

## What Phase 1 decides

| Result | Consequence |
|---|---|
| LoRa passes at rate X | air rate, telemetry period and retry timing for the protocol; gateway location |
| LoRa fails everywhere | antenna upgrade, or Ethernet/WiFi bridge moves from "future" to "now" |
| Moisture sensor yield | how many to reorder; filtering on the analog inputs |
| Relay boot behaviour | driver stage on the IO board |
| Flow accuracy | keep YF-S201 or switch model; per-zone sensors or one on the inlet |
| Valve current/heat | power budget, later choice of latching valves for solar |

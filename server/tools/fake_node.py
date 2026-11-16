#!/usr/bin/env python3
"""Fake field node + gateway on MQTT, for developing the server side with no
hardware. Publishes the same topics and JSON as the real gateway and answers
commands (water / run / stop / clear / auto / rain_skip).

    pip install paho-mqtt
    python3 tools/fake_node.py --host localhost --user garden --password ... [--fast]

--fast publishes telemetry every 5 s instead of every 60 s.
"""
import argparse
import json
import random
import time

import paho.mqtt.client as mqtt

BASE = "garden/node1"
ZONES, FLOW_LPM = 6, 1.0


class Node:
    def __init__(self):
        self.soil = [random.uniform(30, 60) for _ in range(10)]
        self.today = [0.0] * ZONES
        self.queue = []            # [zone, target_l, max_s]
        self.active = None         # [zone, target_l, max_s, delivered_l, started]
        self.auto, self.rain_skip_until, self.event_id = True, 0, 0
        self.start = time.time()

    def telemetry(self):
        return {"t": int(time.time()), "soil": [round(v, 1) for v in self.soil] + [None, None],
                "soil_temp": [18.5, 19.0], "air_temp": round(22 + random.uniform(-1, 1), 1), "air_rh": 55.0,
                "today_l": [round(v, 1) for v in self.today]}

    def status(self):
        st = ["idle"] * ZONES
        for q in self.queue:
            st[q[0] - 1] = "queued"
        if self.active:
            st[self.active[0] - 1] = "dosing"
        return {"t": int(time.time()), "uptime": int(time.time() - self.start), "clock_valid": True, "auto": self.auto,
                "leak": False, "rain_skip": time.time() < self.rain_skip_until, "sim": True,
                "active_zone": self.active[0] if self.active else None, "zone_state": st,
                "zone_fault": ["none"] * ZONES, "valve": [bool(self.active and self.active[0] == z + 1) for z in range(ZONES)],
                "run_l": round(self.active[3], 2) if self.active else 0, "cmd_counter": 0, "reset_reason": 1}

    def event(self, name, **kw):
        self.event_id += 1
        return {"t": int(time.time()), "id": self.event_id, "event": name, **kw}

    def step(self, dt):
        """Advances the simulation; returns the events that happened."""
        events = []
        for i in range(len(self.soil)):
            self.soil[i] = max(5, self.soil[i] - 0.033 * dt / 60)
        if self.active:
            zone, target, max_s, _, started = self.active
            litres = FLOW_LPM * dt / 60
            self.active[3] += litres
            self.today[zone - 1] += litres
            for s in ((zone - 1) * 2, (zone - 1) * 2 + 1):
                if s < len(self.soil):
                    self.soil[s] = min(95, self.soil[s] + 4 * dt / 60)
            done = target and self.active[3] >= target
            if done or time.time() - started >= max_s:
                events.append(self.event("run_finished", zone=zone, litres=round(self.active[3], 2),
                                         seconds=int(time.time() - started),
                                         detail="target_reached" if done else "time_limit"))
                self.active = None
        elif self.queue:
            zone, target, max_s = self.queue.pop(0)
            self.active = [zone, target, max_s, 0.0, time.time()]
            events.append(self.event("run_started", zone=zone, target_l=target, limit_s=max_s))
        return events

    def command(self, d):
        """Returns (ok, result, detail)."""
        cmd, zone = d.get("cmd"), d.get("zone")
        if cmd in ("water", "run"):
            if not isinstance(zone, int) or not 1 <= zone <= ZONES:
                return False, "invalid", "zone"
            if len(self.queue) >= 8:
                return False, "rejected", "queue_full"
            if cmd == "water":
                self.queue.append([zone, float(d.get("litres", 0)), int(d.get("max_s") or 1800)])
            else:
                self.queue.append([zone, 0, int(d.get("seconds", 0))])
        elif cmd == "stop":
            self.queue, self.active = [], None
        elif cmd == "auto":
            self.auto = bool(d.get("on", True))
        elif cmd == "rain_skip":
            self.rain_skip_until = time.time() + 3600 * float(d.get("hours", 0))
        elif cmd != "clear":
            return False, "invalid", "cmd"
        return True, "ok", None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password", default="")
    ap.add_argument("--fast", action="store_true")
    args = ap.parse_args()

    node = Node()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="fake-node")
    if args.user:
        client.username_pw_set(args.user, args.password)
    client.will_set("garden/gateway/availability", "offline", retain=True)

    def on_message(c, u, m):
        try:
            d = json.loads(m.payload)
        except ValueError:
            d = {}
        ok, result, detail = node.command(d)
        out = {"ok": ok, "result": result}
        if d.get("id"):
            out["id"] = d["id"]
        if detail:
            out["detail"] = detail
        c.publish(f"{BASE}/cmd/result", json.dumps(out))
        c.publish(f"{BASE}/status", json.dumps(node.status()), retain=True)
        print("cmd", d, "->", out, flush=True)

    def on_connect(c, u, flags, reason, props):
        print("connected:", reason, flush=True)
        c.publish("garden/gateway/availability", "online", retain=True)
        c.subscribe(f"{BASE}/cmd")

    client.on_connect, client.on_message = on_connect, on_message
    client.connect(args.host, args.port)
    client.loop_start()

    period, last_tm, last_st = (5 if args.fast else 60), 0, 0
    while True:
        time.sleep(1)
        events = node.step(1)
        for e in events:
            client.publish(f"{BASE}/event", json.dumps(e), qos=1)
        now = time.time()
        if events or now - last_st >= 300:
            last_st = now
            client.publish(f"{BASE}/status", json.dumps(node.status()), retain=True)
        if now - last_tm >= period:
            last_tm = now
            client.publish(f"{BASE}/telemetry", json.dumps(node.telemetry()))


if __name__ == "__main__":
    main()

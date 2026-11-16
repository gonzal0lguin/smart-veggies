"""garden-brain: the server-side logic of smart-veggies.

v0 does four things:
  * keeps the latest node state from MQTT and logs it to InfluxDB, tagged with
    bed/zone from garden.yaml (so dashboards are per bed, not per channel);
  * serves the garden map (beds, zones, plantings, live data) on :8080;
  * fetches the Open-Meteo forecast and publishes a rain-skip recommendation;
  * (optional, rules.auto_apply) sends the rain-skip command itself.

Run without a broker to look at the map:  python3 brain.py --demo
"""
import json
import os
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import yaml

HERE = Path(__file__).parent


class Garden:
    """garden.yaml plus the latest data received from the node."""

    def __init__(self, path):
        self.cfg = yaml.safe_load(Path(path).read_text())
        self.node = self.cfg.get("node", "node1")
        self.lock = threading.Lock()
        self.telemetry, self.status, self.gateway = {}, {}, {}
        self.events = []          # newest first, last 50
        self.forecast = None      # {"rain_24h_mm": .., "et0_today_mm": .., "fetched": ..}
        self.recommendation = None
        self.last_seen = 0
        # sensor number -> (bed id, zone number)
        self.sensor_place = {}
        for bed in self.cfg.get("beds", []):
            for z in bed.get("zones", []):
                for s in z.get("soil_sensors", []):
                    self.sensor_place[s] = (bed["id"], z["zone"])

    # ------------------------------------------------------------- MQTT input
    def on_message(self, topic, payload):
        """Returns InfluxDB line-protocol lines for this message."""
        try:
            data = json.loads(payload)
        except ValueError:
            return []
        kind = topic.split("/")[-1]
        with self.lock:
            if topic == "garden/gateway/status":
                self.gateway = data
                return []
            self.last_seen = time.time()
            if kind == "telemetry":
                self.telemetry = data
                return self._telemetry_lines(data)
            if kind == "status":
                self.status = data
            elif kind == "event":
                self.events = ([data] + self.events)[:50]
                return self._event_lines(data)
        return []

    def _telemetry_lines(self, d):
        ts = int(d.get("t") or time.time())
        lines = []
        for i, v in enumerate(d.get("soil", []), start=1):
            if v is None:
                continue
            bed, zone = self.sensor_place.get(i, ("unassigned", 0))
            lines.append(f"soil,node={self.node},bed={bed},zone={zone},sensor={i} moisture={float(v)} {ts}")
        for i, v in enumerate(d.get("soil_temp", []), start=1):
            if v is not None:
                lines.append(f"soil_temp,node={self.node},probe={i} temperature={float(v)} {ts}")
        air = [f"{k}={float(d[src])}" for k, src in (("temperature", "air_temp"), ("humidity", "air_rh")) if d.get(src) is not None]
        if air:
            lines.append(f"air,node={self.node} {','.join(air)} {ts}")
        if d.get("supply_v") is not None:
            lines.append(f"power,node={self.node} supply_v={float(d['supply_v'])} {ts}")
        for i, v in enumerate(d.get("today_l", []), start=1):
            lines.append(f"water_today,node={self.node},zone={i} litres={float(v)} {ts}")
        return lines

    def _event_lines(self, d):
        if d.get("event") not in ("run_finished", "run_fault"):
            return []
        ts = int(d.get("t") or time.time())
        tags = f"node={self.node},zone={d.get('zone', 0)},result={d.get('detail', 'unknown')}"
        return [f"watering,{tags} litres={float(d.get('litres', 0))},seconds={int(d.get('seconds', 0))}i {ts}"]

    # ---------------------------------------------------------------- map API
    def snapshot(self):
        with self.lock:
            soil = self.telemetry.get("soil", [])
            temps = self.telemetry.get("soil_temp", [])
            today = self.telemetry.get("today_l", [])
            states = self.status.get("zone_state", [])
            faults = self.status.get("zone_fault", [])

            def pick(arr, n):
                return arr[n - 1] if n and 0 < n <= len(arr) else None

            beds = []
            for bed in self.cfg.get("beds", []):
                zones = []
                for z in bed.get("zones", []):
                    readings = [pick(soil, s) for s in z.get("soil_sensors", [])]
                    valid = [r for r in readings if r is not None]
                    zones.append({
                        "zone": z["zone"],
                        "name": z.get("name", ""),
                        "sensors": [{"n": s, "moisture": r} for s, r in zip(z.get("soil_sensors", []), readings)],
                        "moisture": round(sum(valid) / len(valid), 1) if valid else None,
                        "today_l": pick(today, z["zone"]),
                        "state": pick(states, z["zone"]) or "unknown",
                        "fault": pick(faults, z["zone"]) or "none",
                        "nominal_lpm": round(z.get("emitters", 0) * z.get("emitter_lph", 0) / 60, 2),
                        "plantings": [{**p, "planted": str(p.get("planted", ""))} for p in z.get("plantings", [])],
                    })
                beds.append({
                    "id": bed["id"], "name": bed.get("name", bed["id"]),
                    "position_m": bed.get("position_m", [0, 0]), "size_m": bed.get("size_m", [2, 1]),
                    "soil_temp": pick(temps, bed.get("soil_temp")), "zones": zones,
                })
            return {
                "site": self.cfg.get("site", {}).get("name", "Garden"),
                "age_s": round(time.time() - self.last_seen) if self.last_seen else None,
                "air_temp": self.telemetry.get("air_temp"), "air_rh": self.telemetry.get("air_rh"),
                "auto": self.status.get("auto"), "leak": self.status.get("leak"), "sim": self.status.get("sim"),
                "rain_skip": self.status.get("rain_skip"),
                "forecast": self.forecast, "recommendation": self.recommendation,
                "events": self.events[:10], "beds": beds,
            }

    # --------------------------------------------------------------- forecast
    def update_forecast(self, fetch=None):
        """Fetches the forecast; returns a recommendation dict or None."""
        site = self.cfg.get("site", {})
        lat, lon = site.get("latitude"), site.get("longitude")
        if lat is None or lon is None:
            return None
        url = ("https://api.open-meteo.com/v1/forecast"
               f"?latitude={lat}&longitude={lon}&hourly=precipitation&daily=et0_fao_evapotranspiration"
               "&forecast_days=2&timezone=auto")
        raw = (fetch or _http_get)(url)
        data = json.loads(raw)
        hours = data["hourly"]
        now = time.strftime("%Y-%m-%dT%H:00", time.localtime())
        start = next((i for i, t in enumerate(hours["time"]) if t >= now), 0)
        rain = round(sum(p or 0 for p in hours["precipitation"][start:start + 24]), 1)
        et0 = (data.get("daily", {}).get("et0_fao_evapotranspiration") or [None])[0]
        limit = float(self.cfg.get("rules", {}).get("rain_skip_mm", 5.0))
        rec = {
            "t": int(time.time()),
            "action": "rain_skip" if rain >= limit else "none",
            "hours": 24 if rain >= limit else 0,
            "reason": f"{rain} mm of rain forecast in the next 24 h (threshold {limit} mm)",
        }
        with self.lock:
            self.forecast = {"rain_24h_mm": rain, "et0_today_mm": et0, "fetched": int(time.time())}
            self.recommendation = rec
        return rec


def _http_get(url, timeout=20):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode()


def influx_write(lines):
    url, token = os.environ.get("INFLUX_URL"), os.environ.get("INFLUX_TOKEN")
    if not lines or not url or not token:
        return
    req = urllib.request.Request(
        f"{url}/api/v2/write?org=garden&bucket=garden&precision=s", data="\n".join(lines).encode(),
        headers={"Authorization": f"Token {token}", "Content-Type": "text/plain"}, method="POST")
    try:
        urllib.request.urlopen(req, timeout=10).close()
    except OSError as e:
        print("influx write failed:", e, flush=True)


def serve(garden, port=8080):
    page = (HERE / "map.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/api/state":
                body, ctype = json.dumps(garden.snapshot()).encode(), "application/json"
            elif self.path in ("/", "/index.html"):
                body, ctype = page, "text/html; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def demo(garden):
    """Fake data so the map can be looked at with no broker and no node."""
    import random
    soil = [round(random.uniform(20, 75), 1) for _ in range(10)] + [None, None]
    base = f"garden/{garden.node}"
    garden.on_message(f"{base}/telemetry", json.dumps({
        "t": int(time.time()), "soil": soil, "soil_temp": [18.4, 19.1], "air_temp": 24.3, "air_rh": 52,
        "today_l": [4, 4, 2.5, 0, 0, 1]}))
    garden.on_message(f"{base}/status", json.dumps({
        "auto": True, "leak": False, "sim": True,
        "zone_state": ["idle", "dosing", "idle", "idle", "queued", "idle"],
        "zone_fault": ["none", "none", "none", "no_flow", "none", "none"]}))
    garden.on_message(f"{base}/event", json.dumps({
        "t": int(time.time()), "event": "run_finished", "zone": 1, "litres": 4.0, "seconds": 241, "detail": "target_reached"}))


def main():
    garden = Garden(os.environ.get("GARDEN_FILE", HERE / "garden.yaml" if (HERE / "garden.yaml").exists() else HERE.parent / "garden.yaml"))
    port = int(os.environ.get("PORT", "8080"))
    serve(garden, port)
    print(f"garden map on http://0.0.0.0:{port}", flush=True)
    if "--demo" in sys.argv:
        demo(garden)
        while True:
            time.sleep(3600)

    import paho.mqtt.client as mqtt
    base = f"garden/{garden.node}"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="garden-brain")
    if os.environ.get("MQTT_USER"):
        client.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASSWORD", ""))

    def on_connect(c, userdata, flags, reason, props):
        print("mqtt connected:", reason, flush=True)
        c.subscribe([(f"{base}/telemetry", 0), (f"{base}/status", 0), (f"{base}/event", 1), ("garden/gateway/status", 0)])

    client.on_connect = on_connect
    client.on_message = lambda c, u, m: influx_write(garden.on_message(m.topic, m.payload))
    client.connect_async(os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883")))
    client.loop_start()

    while True:
        try:
            rec = garden.update_forecast()
            if rec:
                client.publish("garden/brain/recommendation", json.dumps(rec), retain=True)
                if rec["action"] == "rain_skip" and garden.cfg.get("rules", {}).get("auto_apply"):
                    client.publish(f"{base}/cmd", json.dumps({"cmd": "rain_skip", "hours": rec["hours"], "id": "brain"}), qos=1)
        except Exception as e:  # network trouble must never stop logging
            print("forecast failed:", e, flush=True)
        time.sleep(3 * 3600)


if __name__ == "__main__":
    main()

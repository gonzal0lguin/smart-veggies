#!/usr/bin/env python3
"""End-to-end check of the server side with no hardware: MQTT broker + fake node
+ garden-brain. Used by CI (.github/workflows/ci.yml) and runnable by hand:

    docker run -d --rm -p 1883:1883 eclipse-mosquitto:2 \\
        sh -c 'printf "listener 1883\\nallow_anonymous true\\n" > /tmp/m.conf && mosquitto -c /tmp/m.conf'
    pip install -r garden-brain/requirements.txt
    python3 tests/smoke_test.py            # MQTT_HOST / MQTT_PORT to point elsewhere

It proves: the topics and JSON the server expects, a command round trip
(cmd -> cmd/result -> run_started -> run_finished), and that garden-brain turns
telemetry into the map state served on /api/state.
"""
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import paho.mqtt.client as mqtt

SERVER = Path(__file__).resolve().parent.parent
HOST, PORT = os.environ.get("MQTT_HOST", "localhost"), int(os.environ.get("MQTT_PORT", "1883"))
HTTP_PORT = int(os.environ.get("BRAIN_PORT", "18080"))
BASE = "garden/node1"

inbox = queue.Queue()
seen = []


def wait_for(what, match, timeout=30):
    """Returns the first message, already seen or still to come, for which match() is true."""
    deadline = time.time() + timeout
    while True:
        for topic, data in list(seen):
            if match(topic, data):
                print(f"ok   {what}")
                return data
        if time.time() > deadline:
            raise SystemExit(f"FAIL {what}: nothing matching within {timeout} s")
        try:
            seen.append(inbox.get(timeout=0.5))
        except queue.Empty:
            pass


def check(what, cond):
    if not cond:
        raise SystemExit(f"FAIL {what}")
    print(f"ok   {what}")


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="smoke-test")

    def on_message(c, u, m):
        try:
            inbox.put((m.topic, json.loads(m.payload)))
        except ValueError:
            inbox.put((m.topic, m.payload.decode(errors="replace")))

    client.on_message = on_message
    client.on_connect = lambda c, u, f, r, p: c.subscribe("garden/#", qos=1)
    client.connect(HOST, PORT)
    client.loop_start()

    archive_dir = tempfile.mkdtemp(prefix="garden-archive-")
    env = dict(os.environ, MQTT_HOST=HOST, MQTT_PORT=str(PORT), PORT=str(HTTP_PORT),
               GARDEN_FILE=str(SERVER / "garden.yaml"), INFLUX_URL="", ARCHIVE_DIR=archive_dir)
    procs = [
        subprocess.Popen([sys.executable, str(SERVER / "tools/fake_node.py"), "--host", HOST, "--port", str(PORT), "--fast"]),
        subprocess.Popen([sys.executable, str(SERVER / "garden-brain/brain.py")], env=env),
    ]
    try:
        t = wait_for("telemetry published", lambda tp, d: tp == f"{BASE}/telemetry")
        check("telemetry has 12 soil channels, 6 zone totals",
              len(t.get("soil", [])) == 12 and len(t.get("today_l", [])) == 6 and "air_temp" in t)
        s = wait_for("status published", lambda tp, d: tp == f"{BASE}/status")
        check("status has 6 zone states and faults", len(s.get("zone_state", [])) == 6 and len(s.get("zone_fault", [])) == 6)

        client.publish(f"{BASE}/cmd", json.dumps({"cmd": "water", "zone": 2, "litres": 0.05, "id": "smoke-1"}), qos=1)
        r = wait_for("command acknowledged", lambda tp, d: tp == f"{BASE}/cmd/result" and d.get("id") == "smoke-1")
        check("command accepted", r.get("ok") is True and r.get("result") == "ok")
        wait_for("run started on zone 2", lambda tp, d: tp == f"{BASE}/event" and d.get("event") == "run_started" and d.get("zone") == 2)
        e = wait_for("run finished on zone 2", lambda tp, d: tp == f"{BASE}/event" and d.get("event") == "run_finished" and d.get("zone") == 2)
        check("run finished because the target was reached", e.get("detail") == "target_reached" and e.get("litres", 0) >= 0.05)

        client.publish(f"{BASE}/cmd", json.dumps({"cmd": "water", "zone": 9, "litres": 1, "id": "smoke-2"}), qos=1)
        r = wait_for("bad zone answered", lambda tp, d: tp == f"{BASE}/cmd/result" and d.get("id") == "smoke-2")
        check("bad zone refused", r.get("ok") is False)

        wait_for("telemetry after the run", lambda tp, d: tp == f"{BASE}/telemetry" and d["today_l"][1] > 0, timeout=20)

        # History caught up after an outage: stored, but never shown as the current value.
        old_t = int(time.time()) - 86400
        client.publish(f"{BASE}/telemetry/backfill", json.dumps({"t": old_t, "soil": [1.0] * 12, "soil_temp": [None, None],
                                                                 "today_l": [0] * 6}), qos=1)
        time.sleep(1.5)
        with urllib.request.urlopen(f"http://localhost:{HTTP_PORT}/api/state", timeout=5) as resp:
            state = json.load(resp)
        zones = [z for b in state["beds"] for z in b["zones"]]
        check("garden map has 3 beds and 6 zones", len(state["beds"]) == 3 and len(zones) == 6)
        z2 = next(z for z in zones if z["zone"] == 2)
        check("map shows zone 2 moisture and litres today", z2["moisture"] is not None and z2["today_l"] > 0)
        with urllib.request.urlopen(f"http://localhost:{HTTP_PORT}/", timeout=5) as resp:
            check("map page is served", resp.status == 200 and b"<html" in resp.read().lower())
        check("backfilled telemetry did not replace the live reading", z2["moisture"] > 2)
        check("fake node and garden-brain still running", all(p.poll() is None for p in procs))

        files = sorted(Path(archive_dir).rglob("*.jsonl"))
        rows = [json.loads(line) for f in files for line in f.read_text().splitlines()]
        topics = {r["topic"] for r in rows}
        check("raw archive has telemetry, events, commands and the backfilled sample",
              {f"{BASE}/telemetry", f"{BASE}/event", f"{BASE}/cmd", f"{BASE}/telemetry/backfill"} <= topics)
        out = Path(archive_dir) / "export"
        subprocess.run([sys.executable, str(SERVER / "tools/export_dataset.py"), archive_dir, str(out)], check=True)
        tele = (out / "telemetry.csv").read_text().splitlines()
        events = (out / "events.csv").read_text()
        check("dataset export: telemetry sorted by time with the backfilled row first",
              len(tele) >= 3 and tele[0].startswith("t,") and tele[1].split(",")[0] == str(old_t))
        check("dataset export: events table has the finished run", "run_finished" in events)
    finally:
        for p in procs:
            p.terminate()
        client.loop_stop()
        shutil.rmtree(archive_dir, ignore_errors=True)
    print("smoke test passed")


if __name__ == "__main__":
    main()

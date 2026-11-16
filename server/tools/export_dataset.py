#!/usr/bin/env python3
"""Turns the raw message archive into tidy tables for analysis and model training.

    python3 tools/export_dataset.py <archive_dir> <out_dir> [--from 2026-11-01] [--to 2026-12-01]

Writes (standard library only; load the results with pandas, R, a spreadsheet...):
    telemetry.csv   one row per sample: t (UTC epoch), time (ISO), soil_1..N (%),
                    soil_temp_1..N (C), air_temp, air_rh, supply_v, today_l_1..N
    events.csv      one row per event: watering runs with litres/seconds, faults, boots

Rows are sorted by the node's own timestamp and de-duplicated, so data that
arrived late (backfill after an outage) sits where it belongs.
"""
import csv
import json
import sys
import time
from pathlib import Path

EVENT_FIELDS = ["id", "event", "zone", "flow_sensor", "target_l", "limit_s", "litres", "seconds", "detail", "reset_reason"]


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def read_archive(directory, first=None, last=None):
    for f in sorted(Path(directory).rglob("*.jsonl")):
        if (first and f.stem < first) or (last and f.stem > last):
            continue
        with open(f) as fh:
            for line in fh:
                try:
                    row = json.loads(line)
                except ValueError:
                    continue  # a line cut short by a power failure
                if isinstance(row.get("data"), dict):
                    yield row["topic"], row["data"], row.get("rx")


def export(archive_dir, out_dir, first=None, last=None):
    telemetry, events = {}, {}
    for topic, d, rx in read_archive(archive_dir, first, last):
        parts = topic.split("/")
        if parts[-1] == "backfill":
            parts.pop()
        if len(parts) != 3:
            continue
        node, kind = parts[1], parts[2]
        t = int(d.get("t") or rx or 0)
        if kind == "telemetry":
            telemetry[(node, t)] = d
        elif kind == "event":
            events[(node, t, d.get("id"), d.get("event"))] = d

    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)

    def width(key):
        return max((len(d.get(key) or []) for d in telemetry.values()), default=0)

    n_soil, n_temp, n_zone = width("soil"), width("soil_temp"), width("today_l")
    with open(out / "telemetry.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["t", "time", "node"] + [f"soil_{i + 1}" for i in range(n_soil)] + [f"soil_temp_{i + 1}" for i in range(n_temp)]
                   + ["air_temp", "air_rh", "supply_v"] + [f"today_l_{i + 1}" for i in range(n_zone)])
        for (node, t), d in sorted(telemetry.items(), key=lambda kv: (kv[0][1], kv[0][0])):
            def cells(key, n):
                v = d.get(key) or []
                return ["" if i >= len(v) or v[i] is None else v[i] for i in range(n)]
            w.writerow([t, iso(t), node] + cells("soil", n_soil) + cells("soil_temp", n_temp)
                       + [d.get(k, "") for k in ("air_temp", "air_rh", "supply_v")] + cells("today_l", n_zone))

    with open(out / "events.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["t", "time", "node"] + EVENT_FIELDS)
        for (node, t, _, _), d in sorted(events.items(), key=lambda kv: (kv[0][1], kv[0][0], str(kv[0][2]))):
            w.writerow([t, iso(t), node] + ["" if d.get(k) is None else d.get(k) for k in EVENT_FIELDS])
    return len(telemetry), len(events)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        raise SystemExit(__doc__)

    def opt(name):
        return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None

    n_t, n_e = export(args[0], args[1], opt("--from"), opt("--to"))
    print(f"{n_t} telemetry rows, {n_e} events -> {args[1]}")


if __name__ == "__main__":
    main()

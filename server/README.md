# Server stack

Runs on the Raspberry Pi 4 (64-bit OS, booting from SSD): Mosquitto, Home
Assistant, InfluxDB, Grafana and `garden-brain` (our own service: data logging
by bed/zone, garden map, forecast-based recommendations).

Status: the compose file and the MQTT path have not been run end to end yet.
`garden-brain` was only checked in demo mode.

## First start

```bash
cp .env.example .env                 # set every password/token
touch mosquitto/passwd
docker compose run --rm mosquitto mosquitto_passwd -b /mosquitto/config/passwd garden '<MQTT_PASSWORD from .env>'
docker compose up -d
```

Then:

1. **Home Assistant** at `http://<pi>:8123`: Settings → Devices & services → add
   **MQTT**, broker `127.0.0.1`, port 1883, user `garden`. The gateway announces
   its entities by itself (device "Garden node1").
2. **Grafana** at `http://<pi>:3000`: add an InfluxDB data source, query language
   Flux, URL `http://influxdb:8086`, organisation `garden`, bucket `garden`, token
   from `.env`. Measurements: `soil` (tags bed, zone, sensor), `soil_temp`, `air`,
   `water_today`, `watering`, `power`.
3. **Garden map** at `http://<pi>:8080`. To show it inside Home Assistant, add a
   "Webpage" card pointing at that URL.
4. Put the same MQTT user/password and the Pi's address in the gateway's
   `firmware/include/secrets.h`.

## garden.yaml

Describes beds, zones, which moisture sensor sits where and what is planted.
Edit it when you plant or move something, then `docker compose restart garden-brain`.
Setting `site.latitude/longitude` enables the forecast; the coordinates are sent
only to Open-Meteo.

## Without hardware

```bash
python3 garden-brain/brain.py --demo          # map with fake data, no broker (needs PyYAML)
pip install paho-mqtt
python3 tools/fake_node.py --user garden --password '<...>' --fast   # pretend node + gateway
mosquitto_pub -u garden -P '<...>' -t garden/node1/cmd -m '{"cmd":"water","zone":1,"litres":2}'
```

## Remote access and backups

Install Tailscale on the Pi for access from outside (no port forwarding).
Back up the docker volumes `ha-config`, `influx-data` and `grafana-data` nightly.

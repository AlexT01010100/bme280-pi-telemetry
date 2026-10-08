# pi-air-monitor

[![CI](https://github.com/AlexT01010100/pi-air-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/AlexT01010100/pi-air-monitor/actions/workflows/ci.yml)

Raspberry Pi 5 + BME280 (SPI) and MH-Z19 CO₂ (UART) sensors → C drivers → FastAPI service → Postgres → web dashboard.

![Live dashboard showing 24 hours of temperature, pressure, humidity and CO₂ readings](docs/dashboard.png)

```mermaid
flowchart LR
    subgraph pi["Raspberry Pi 5"]
        sensor["BME280<br/>sensor"] -- "SPI<br/>/dev/spidev0.0" --> driver["C driver<br/>libbme280.so"]
        driver -- ctypes --> sampler["FastAPI service<br/>sampler + REST API"]
        co2["MH-Z19<br/>CO₂ sensor"] -- "UART<br/>/dev/ttyAMA0" --> co2drv["C driver<br/>libmhz19.so"]
        co2drv -- ctypes --> sampler
        sampler -- "insert / query" --> db[("PostgreSQL")]
        sampler -- "JSON" --> dash["Web dashboard"]
    end
    subgraph gh["GitHub"]
        ci["Actions CI<br/>build · test · arm64 image"] --> ghcr["GHCR<br/>container image"]
    end
    ghcr -. "pull (deploy/update.sh)" .-> sampler
```

```
bme280_spi.c / bme280.h   C driver (SPI, calibration, compensation)
  ├─ ./bme280_spi          standalone CLI  (make bme280_spi)
  └─ libbme280.so          shared library  (make libbme280.so)  ← loaded by Python via ctypes
mhz19_uart.c / mhz19.h    C driver (UART/termios, framing, checksum, timeouts)
  ├─ ./mhz19               standalone CLI  (make mhz19)
  └─ libmhz19.so           shared library  (make libmhz19.so)   ← loaded by Python via ctypes
tests/                     C unit tests for both drivers (make test)
service/
  app/driver.py            ctypes bindings + mock sensors
  app/sampler.py           background loop: read both sensors every N s → insert into Postgres
  app/levels.py            CO₂ danger bands + level detector (with hysteresis)
  app/api.py               REST API
  app/static/index.html    dashboard (no external dependencies; works offline)
docker-compose.yml         Postgres + API
deploy/                    systemd unit for a non-Docker install
```

## Quick start on the Pi (Docker)

```bash
sudo raspi-config nonint do_spi 0     # enable SPI once
# for the CO2 sensor: add dtparam=uart0=on to /boot/firmware/config.txt,
# and turn off the serial login console (raspi-config → Interface Options → Serial Port), then reboot
git clone https://github.com/AlexT01010100/pi-air-monitor.git
cd pi-air-monitor
make                                  # optional: builds the CLIs + .so files locally
./bme280_spi                          # sanity-check the wiring
./mhz19                               # ...and the CO2 sensor
docker compose pull && docker compose up -d   # or: docker compose up -d --build
```

Open `http://<pi-address>:8000/`. API docs are at `/docs`.

## Quick start on the Pi (native, no Docker)

```bash
sudo apt install postgresql python3-venv build-essential
sudo -u postgres psql -c "CREATE USER bme PASSWORD 'bme'" -c "CREATE DATABASE telemetry OWNER bme"
make libbme280.so libmhz19.so
cd service
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
cp .env.example .env                 # edit as needed
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

To run it at boot, use `deploy/bme-telemetry.service`.

## Developing off the Pi

Set `BME_DRIVER=mock` (and `BME_CO2_DRIVER=mock`) to generate synthetic readings. The mock is never used
automatically: in `native` mode a missing library or sensor is reported as an
error, so fake data cannot end up in a production database.

```bash
docker run -d --name pg -p 5432:5432 -e POSTGRES_USER=bme -e POSTGRES_PASSWORD=bme -e POSTGRES_DB=telemetry postgres:16-alpine
cd service && pip install -r requirements-dev.txt
BME_DRIVER=mock BME_CO2_DRIVER=mock uvicorn app.main:app --reload
```

## API

| Method | Path | |
|---|---|---|
| GET  | `/api/health` | DB + sensor status (BME280 and CO₂), last error, chip ID, current CO₂ level |
| GET  | `/api/levels/co2` | CO₂ danger bands (thresholds, labels, advice) |
| GET  | `/api/readings/latest` | most recent stored reading |
| GET  | `/api/readings?start=&end=&limit=` | raw readings, newest first |
| GET  | `/api/readings/series?hours=24&points=300` | time-bucketed averages for charts |
| GET  | `/api/readings/stats?hours=24` | min / max / avg / count |
| GET  | `/api/readings.csv?hours=24` | CSV export |
| POST | `/api/readings/sample` | read the sensors now and store the reading (503 if the BME280 fails) |

All read endpoints accept `sensor_id` (default `BME_SENSOR_ID`).

## Configuration

All settings are `BME_*` environment variables; see `service/.env.example`.

## Tests

**C drivers** (no hardware needed):

- **BME280:** checks calibration parsing, including the signed H4/H5 fields, the
  Bosch datasheet worked example, and agreement with the datasheet's
  floating-point formulas across the sensor's full operating range.
- **MH-Z19:** checks frame decoding and checksums, then runs the real serial code
  against a pseudo-terminal with a thread playing the sensor. That covers the
  termios setup, replies split across reads, resyncing after line noise,
  timeouts, and corrupted bytes.

```bash
make test
```

**Service:**

```bash
cd service
TEST_DATABASE_URL=postgresql+asyncpg://bme:bme@localhost:5432/telemetry pytest
# On the Pi, also read the real sensors through the ctypes bindings:
BME_TEST_SPI_DEVICE=/dev/spidev0.0 BME_TEST_CO2_DEVICE=/dev/ttyAMA0 TEST_DATABASE_URL=... pytest
```

Note: the test suite truncates the `readings` table, so point it at a separate database.

## CI/CD

Every push and pull request runs [CI](.github/workflows/ci.yml):

1. **C drivers**: compile with `-Werror` for x86_64 and aarch64 (Raspberry Pi) and run the C unit tests.
2. **Service tests**: builds `libbme280.so` and `libmhz19.so` and runs pytest against Postgres, including the ctypes binding tests.
3. **Docker image**: builds for `linux/arm64` and `linux/amd64`. On `main` (and `v*` tags) it is published to
   `ghcr.io/alext01010100/pi-air-monitor`; pull requests build it without publishing.

The Pi pulls new images rather than GitHub pushing to it (GitHub can't reach a Pi on a home network).
To update by hand or automatically:

```bash
bash deploy/update.sh                                     # pull latest image + restart if changed
crontab -e                                                # or check every 15 minutes:
*/15 * * * * bash ~/pi-air-monitor/deploy/update.sh >> ~/bme-update.log 2>&1
```

## Wiring

<img src="docs/hardware.jpg" alt="Raspberry Pi 5 wired to an Adafruit BME280 breakout on a breadboard over SPI, and to an MH-Z19 CO₂ sensor over UART" width="420">

SPI0 on the Pi 5 header, sensor CS on CE0 (`/dev/spidev0.0`). If you wired CS to
CE1, set `BME_SPI_DEVICE=/dev/spidev0.1`.

| BME280 | Pi pin |
|---|---|
| VIN | 3.3V (pin 1) |
| GND | GND (pin 6) |
| SCK | SCLK / GPIO11 (pin 23) |
| SDO | MISO / GPIO9 (pin 21) |
| SDI | MOSI / GPIO10 (pin 19) |
| CS  | CE0 / GPIO8 (pin 24) |

The MH-Z19 CO₂ sensor uses the header UART. On a Pi 5 that is `/dev/ttyAMA0`
after `dtparam=uart0=on`. `/dev/serial0` points at the separate debug
connector (`ttyAMA10`), so don't use it. TX and RX cross over:

| MH-Z19 | Pi pin |
|---|---|
| Vin | 5V (pin 2) |
| GND | GND (pin 14) |
| TX  | RXD / GPIO15 (pin 10) |
| RX  | TXD / GPIO14 (pin 8) |

The CO₂ sensor is optional (`BME_CO2_DRIVER=none` turns it off). If it fails,
BME280 readings are still stored with `co2_ppm` empty, and `/api/health`
reports the CO₂ error. Readings take about 3 minutes to settle after power-on.

## Behaviour notes

- **Error recovery:** if a read fails, that sensor's device is closed and
  reopened on the next tick, so unplugging and reconnecting either sensor
  recovers without restarting the service.
- **Sanity checks:** readings outside the BME280's rated range (−40–85 °C,
  300–1100 hPa, 0–100 %) are rejected as bad SPI reads instead of being stored.
  CO₂ frames are checksum-verified, and values above 10 000 ppm are rejected.
- **Retention:** rows older than `BME_RETENTION_DAYS` are pruned every hour.
- **CO₂ danger levels:** each CO₂ reading is placed in a band. The dashboard
  shows the current band on the CO₂ tile and draws the boundaries on the chart.
  Unhealthy and Dangerous also show an alert banner and log a warning.

  | Level | From | Why |
  |---|---|---|
  | Good | 0 ppm | outdoor air is about 420 ppm |
  | Moderate | 800 ppm | typical of an occupied, ventilated room |
  | Poor | 1200 ppm | drowsiness and poorer concentration are reported |
  | Unhealthy | 2000 ppm | headaches become common |
  | Dangerous | 5000 ppm | OSHA 8-hour workplace exposure limit |

  The level rises as soon as a reading crosses a boundary, but only drops once
  CO₂ is 50 ppm below it, so a reading hovering at a boundary doesn't make the
  level flicker. Bands live in `service/app/levels.py`.

## Contributing

Issues and pull requests are welcome. You don't need the hardware: run the
service with `BME_DRIVER=mock BME_CO2_DRIVER=mock` and the test suite against any Postgres
(see [Tests](#tests)). For bug reports, please include your Pi model, OS
version, and the output of `/api/health`.

## License

[MIT](LICENSE) © 2026 Alexander Terry

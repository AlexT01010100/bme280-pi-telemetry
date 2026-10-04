# bme280-pi-telemetry

[![CI](https://github.com/AlexT01010100/bme280-pi-telemetry/actions/workflows/ci.yml/badge.svg)](https://github.com/AlexT01010100/bme280-pi-telemetry/actions/workflows/ci.yml)

Raspberry Pi 5 + Adafruit BME280 (SPI) → C driver → FastAPI service → Postgres → web dashboard.

![Live dashboard showing temperature, pressure and humidity from the sensor](docs/dashboard.png)

```mermaid
flowchart LR
    subgraph pi["Raspberry Pi 5"]
        sensor["BME280<br/>sensor"] -- "SPI<br/>/dev/spidev0.0" --> driver["C driver<br/>libbme280.so"]
        driver -- ctypes --> sampler["FastAPI service<br/>sampler + REST API"]
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
tests/                     C unit tests for calibration + compensation (make test)
service/
  app/driver.py            ctypes bindings + mock sensor
  app/sampler.py           background loop: read sensor every N s → insert into Postgres
  app/api.py               REST API
  app/static/index.html    dashboard (no external dependencies; works offline)
docker-compose.yml         Postgres + API
deploy/                    systemd unit for a non-Docker install
```

## Quick start on the Pi (Docker)

```bash
sudo raspi-config nonint do_spi 0     # enable SPI once, then reboot
git clone https://github.com/AlexT01010100/bme280-pi-telemetry.git
cd bme280-pi-telemetry
make                                  # optional: builds the CLI + .so locally
./bme280_spi                          # sanity-check the wiring
docker compose pull && docker compose up -d   # or: docker compose up -d --build
```

Open `http://<pi-address>:8000/`. API docs are at `/docs`.

## Quick start on the Pi (native, no Docker)

```bash
sudo apt install postgresql python3-venv build-essential
sudo -u postgres psql -c "CREATE USER bme PASSWORD 'bme'" -c "CREATE DATABASE telemetry OWNER bme"
make libbme280.so
cd service
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
cp .env.example .env                 # edit as needed
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

To run it at boot, use `deploy/bme-telemetry.service`.

## Developing off the Pi

Set `BME_DRIVER=mock` to generate synthetic readings. The mock is never used
automatically: in `native` mode a missing library or sensor is reported as an
error, so fake data cannot end up in a production database.

```bash
docker run -d --name pg -p 5432:5432 -e POSTGRES_USER=bme -e POSTGRES_PASSWORD=bme -e POSTGRES_DB=telemetry postgres:16-alpine
cd service && pip install -r requirements-dev.txt
BME_DRIVER=mock uvicorn app.main:app --reload
```

## API

| Method | Path | |
|---|---|---|
| GET  | `/api/health` | DB + sensor status, last error, chip ID |
| GET  | `/api/readings/latest` | most recent stored reading |
| GET  | `/api/readings?start=&end=&limit=` | raw readings, newest first |
| GET  | `/api/readings/series?hours=24&points=300` | time-bucketed averages for charts |
| GET  | `/api/readings/stats?hours=24` | min / max / avg / count |
| GET  | `/api/readings.csv?hours=24` | CSV export |
| POST | `/api/readings/sample` | read the sensor now and store it (503 on sensor error) |

All read endpoints accept `sensor_id` (default `BME_SENSOR_ID`).

## Configuration

All settings are `BME_*` environment variables; see `service/.env.example`.

## Tests

**C driver** (no hardware needed): checks calibration parsing, including the
signed H4/H5 fields, the Bosch datasheet worked example, and agreement with the
datasheet's floating-point formulas across the sensor's full operating range.

```bash
make test
```

**Service:**

```bash
cd service
TEST_DATABASE_URL=postgresql+asyncpg://bme:bme@localhost:5432/telemetry pytest
# On the Pi, also read the real sensor through the ctypes bindings:
BME_TEST_SPI_DEVICE=/dev/spidev0.0 TEST_DATABASE_URL=... pytest
```

Note: the test suite truncates the `readings` table, so point it at a separate database.

## CI/CD

Every push and pull request runs [CI](.github/workflows/ci.yml):

1. **C driver**: compiles with `-Werror` for x86_64 and aarch64 (Raspberry Pi) and runs the C unit tests.
2. **Service tests**: builds `libbme280.so` and runs pytest against Postgres, including the ctypes binding tests.
3. **Docker image**: builds for `linux/arm64` and `linux/amd64`. On `main` (and `v*` tags) it is published to
   `ghcr.io/alext01010100/bme280-pi-telemetry`; pull requests build it without publishing.

The Pi pulls new images rather than GitHub pushing to it (GitHub can't reach a Pi on a home network).
To update by hand or automatically:

```bash
bash deploy/update.sh                                     # pull latest image + restart if changed
crontab -e                                                # or check every 15 minutes:
*/15 * * * * bash ~/bme280-pi-telemetry/deploy/update.sh >> ~/bme-update.log 2>&1
```

## Wiring

<img src="docs/hardware.jpg" alt="Raspberry Pi 5 wired to an Adafruit BME280 breakout on a breadboard over SPI" width="360">

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

## Behaviour notes

- **Error recovery:** if a read fails, the C device is closed and initialised
  again on the next tick, so unplugging and reconnecting the sensor recovers
  without restarting the service.
- **Sanity checks:** readings outside the BME280's rated range (−40–85 °C,
  300–1100 hPa, 0–100 %) are rejected as bad SPI reads instead of being stored.
- **Retention:** rows older than `BME_RETENTION_DAYS` are pruned every hour.

## Contributing

Issues and pull requests are welcome. You don't need the hardware: run the
service with `BME_DRIVER=mock` and the test suite against any Postgres
(see [Tests](#tests)). For bug reports, please include your Pi model, OS
version, and the output of `/api/health`.

## License

[MIT](LICENSE) © 2026 Alexander Terry

from datetime import datetime, timedelta, timezone

from app.db import ReadingRow
from app.driver import Reading, SensorError


async def insert(app, ts, t=20.0, p=1000.0, h=40.0, co2=None, sensor_id="bme280-0"):
    async with app.state.sessions() as s:
        s.add(ReadingRow(ts=ts, sensor_id=sensor_id, temperature_c=t, pressure_hpa=p, humidity_pct=h, co2_ppm=co2))
        await s.commit()


async def test_latest_404_when_empty(client):
    r = await client.get("/api/readings/latest")
    assert r.status_code == 404


async def test_sample_now_stores_reading(client):
    r = await client.post("/api/readings/sample")
    assert r.status_code == 201
    body = r.json()
    assert -40 <= body["temperature_c"] <= 85
    assert 0 <= body["co2_ppm"] <= 10_000
    latest = (await client.get("/api/readings/latest")).json()
    assert latest["ts"] == body["ts"]


async def test_sample_now_sensor_error_returns_503(app, client):
    class Broken:
        name = "broken"
        def read(self):
            raise SensorError("init /dev/spidev0.0: could not open SPI device")
        def chip_id(self):
            return None
        def close(self):
            pass

    app.state.sampler.sensor = Broken()
    r = await client.post("/api/readings/sample")
    assert r.status_code == 503
    assert "could not open" in r.json()["detail"]
    h = (await client.get("/api/health")).json()
    assert h["status"] == "degraded"
    assert h["sensor"]["consecutive_errors"] == 1


async def test_co2_failure_still_stores_bme_reading(app, client):
    class BrokenCo2:
        name = "broken"
        def read(self):
            raise SensorError("/dev/ttyAMA0: got 0/9 bytes")
        def close(self):
            pass

    app.state.sampler.co2 = BrokenCo2()
    r = await client.post("/api/readings/sample")
    assert r.status_code == 201
    assert r.json()["co2_ppm"] is None and r.json()["temperature_c"] is not None
    h = (await client.get("/api/health")).json()
    assert h["status"] == "degraded"
    assert h["sensor"]["last_error"] is None
    assert h["co2"]["consecutive_errors"] == 1 and "0/9 bytes" in h["co2"]["last_error"]


async def test_list_filters_by_window_and_sensor(app, client):
    now = datetime.now(timezone.utc)
    await insert(app, now - timedelta(hours=3), t=10)
    await insert(app, now - timedelta(minutes=5), t=11)
    await insert(app, now, t=99, sensor_id="other")
    r = await client.get("/api/readings", params={"start": (now - timedelta(hours=1)).isoformat()})
    assert [x["temperature_c"] for x in r.json()] == [11]
    r = await client.get("/api/readings", params={"sensor_id": "other"})
    assert [x["temperature_c"] for x in r.json()] == [99]


async def test_series_buckets_average(app, client):
    base = datetime.now(timezone.utc).replace(second=0, microsecond=0) - timedelta(minutes=30)
    # hours=1, points=60 -> 60 s buckets; two readings in one bucket average together
    await insert(app, base + timedelta(seconds=5), t=20, p=1000, h=40, co2=600)
    await insert(app, base + timedelta(seconds=35), t=22, p=1002, h=42)  # CO2 missing: averaged over the rest
    await insert(app, base + timedelta(minutes=5), t=30, p=1010, h=50)
    s = (await client.get("/api/readings/series", params={"hours": 1, "points": 60})).json()
    assert s["bucket_s"] == 60
    pts = s["points"]
    assert len(pts) == 2
    assert pts[0]["temperature_c"] == 21.0 and pts[0]["pressure_hpa"] == 1001.0
    assert pts[0]["co2_ppm"] == 600.0
    assert pts[1]["humidity_pct"] == 50.0 and pts[1]["co2_ppm"] is None
    assert pts[0]["ts"] < pts[1]["ts"]


async def test_stats(app, client):
    now = datetime.now(timezone.utc)
    for i, (t, co2) in enumerate([(18.0, 500), (20.0, None), (25.0, 900)]):
        await insert(app, now - timedelta(minutes=i), t=t, co2=co2)
    st = (await client.get("/api/readings/stats", params={"hours": 1})).json()
    assert st["count"] == 3
    assert st["temperature_c"] == {"min": 18.0, "max": 25.0, "avg": 21.0}
    assert st["co2_ppm"] == {"min": 500.0, "max": 900.0, "avg": 700.0}


async def test_stats_empty(client):
    st = (await client.get("/api/readings/stats")).json()
    assert st["count"] == 0 and st["humidity_pct"]["avg"] is None


async def test_csv_export(app, client):
    now = datetime.now(timezone.utc)
    await insert(app, now - timedelta(minutes=2), t=19.5, co2=812)
    await insert(app, now - timedelta(minutes=1), t=19.75)
    r = await client.get("/api/readings.csv", params={"hours": 1})
    assert r.status_code == 200
    lines = r.text.strip().splitlines()
    assert lines[0] == "ts,sensor_id,temperature_c,pressure_hpa,humidity_pct,co2_ppm"
    assert len(lines) == 3 and lines[1].split(",")[2] == "19.5"
    assert lines[1].split(",")[-1] == "812.0" and lines[2].split(",")[-1] == ""


async def test_prune_respects_retention(app):
    now = datetime.now(timezone.utc)
    await insert(app, now - timedelta(days=40))
    await insert(app, now - timedelta(days=1))
    assert await app.state.sampler.prune() == 1


async def test_dashboard_served(client):
    r = await client.get("/")
    assert r.status_code == 200 and "Indoor Air Monitor" in r.text


def test_reading_validation_rejects_garbage():
    import pytest
    # All-zero SPI frames (MISO floating low) decode to values like these
    with pytest.raises(SensorError):
        Reading(temperature_c=-142.0, pressure_hpa=1000.0, humidity_pct=40.0).validate()
    assert Reading(21.0, 1013.0, 45.0).validate().temperature_c == 21.0

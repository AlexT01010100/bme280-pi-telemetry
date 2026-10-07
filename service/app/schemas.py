from datetime import datetime

from pydantic import BaseModel, ConfigDict


class ReadingOut(BaseModel):
    model_config = ConfigDict(from_attributes=True)

    ts: datetime
    sensor_id: str
    temperature_c: float
    pressure_hpa: float
    humidity_pct: float
    co2_ppm: float | None = None


class SeriesPoint(BaseModel):
    ts: datetime
    temperature_c: float
    pressure_hpa: float
    humidity_pct: float
    co2_ppm: float | None = None


class Series(BaseModel):
    sensor_id: str
    start: datetime
    end: datetime
    bucket_s: int
    points: list[SeriesPoint]


class FieldStats(BaseModel):
    min: float | None
    max: float | None
    avg: float | None


class Stats(BaseModel):
    sensor_id: str
    start: datetime
    end: datetime
    count: int
    temperature_c: FieldStats
    pressure_hpa: FieldStats
    humidity_pct: FieldStats
    co2_ppm: FieldStats


class SensorHealth(BaseModel):
    driver: str
    chip_id: str | None
    sampler_running: bool
    last_ok_at: datetime | None
    last_error: str | None
    consecutive_errors: int


class Co2Health(BaseModel):
    driver: str
    last_ok_at: datetime | None
    last_error: str | None
    consecutive_errors: int


class Health(BaseModel):
    status: str  # "ok" | "degraded"
    database: bool
    sensor: SensorHealth
    co2: Co2Health | None  # None when no CO2 sensor is configured
    sample_interval_s: float

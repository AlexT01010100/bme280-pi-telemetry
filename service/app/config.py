from functools import lru_cache
from pathlib import Path
from typing import Literal

from pydantic_settings import BaseSettings, SettingsConfigDict

REPO_ROOT = Path(__file__).resolve().parents[2]


class Settings(BaseSettings):
    """All settings can be overridden with BME_* environment variables or a .env file."""

    model_config = SettingsConfigDict(env_prefix="BME_", env_file=".env", extra="ignore")

    database_url: str = "postgresql+asyncpg://bme:bme@localhost:5432/telemetry"

    # "native" loads libbme280.so via ctypes; "mock" generates synthetic data for
    # development off the Pi. There is deliberately no automatic fallback, so a
    # broken driver never silently fills the database with fake readings.
    driver: Literal["native", "mock"] = "native"
    lib_path: Path = REPO_ROOT / "libbme280.so"
    spi_device: str = "/dev/spidev0.0"
    spi_speed_hz: int = 500_000

    sensor_id: str = "bme280-0"
    sample_interval_s: float = 10.0
    sampler_enabled: bool = True

    # Rows older than this are pruned hourly; 0 keeps everything.
    retention_days: int = 30


@lru_cache
def get_settings() -> Settings:
    return Settings()

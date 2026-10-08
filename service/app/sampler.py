import asyncio
import logging
import time
from datetime import datetime, timedelta, timezone

from sqlalchemy import delete
from sqlalchemy.ext.asyncio import async_sessionmaker

from .config import Settings
from .db import ReadingRow
from .driver import Co2Sensor, Sensor, SensorError
from .levels import Co2Detector

log = logging.getLogger(__name__)

PRUNE_EVERY_S = 3600


class Sampler:
    """Polls the sensor on a fixed interval and stores each reading."""

    def __init__(
        self, sensor: Sensor, sessions: async_sessionmaker, settings: Settings, co2: Co2Sensor | None = None
    ):
        self.sensor = sensor
        self.co2 = co2
        self.sessions = sessions
        self.settings = settings
        self.last_ok_at: datetime | None = None
        self.last_error: str | None = None
        self.consecutive_errors = 0
        self.co2_last_ok_at: datetime | None = None
        self.co2_last_error: str | None = None
        self.co2_consecutive_errors = 0
        self.co2_detector = Co2Detector()
        self._last_prune = 0.0

    async def _read_co2(self) -> float | None:
        """CO2 is optional: a failed read is recorded, and the row is stored with co2_ppm NULL."""
        if self.co2 is None:
            return None
        try:
            ppm = await asyncio.to_thread(self.co2.read)
        except SensorError as e:
            self.co2_last_error = str(e)
            self.co2_consecutive_errors += 1
            if self.co2_consecutive_errors == 1 or self.co2_consecutive_errors % 30 == 0:
                log.warning("co2 read failed (%d in a row): %s", self.co2_consecutive_errors, e)
            return None
        self.co2_last_ok_at = datetime.now(timezone.utc)
        self.co2_last_error = None
        self.co2_consecutive_errors = 0
        self._track_co2_level(ppm)
        return float(ppm)

    def _track_co2_level(self, ppm: float) -> None:
        change = self.co2_detector.update(ppm, self.co2_last_ok_at)
        if change is None:
            return
        previous, level = change
        transition = f"{previous.label} -> {level.label}" if previous else level.label
        log.log(
            logging.WARNING if level.alert else logging.INFO,
            "CO2 level %s at %d ppm: %s", transition, ppm, level.advice,
        )

    async def sample_once(self) -> ReadingRow:
        # The C driver blocks on ioctl/usleep, so keep it off the event loop.
        try:
            reading = await asyncio.to_thread(self.sensor.read)
        except SensorError as e:
            self.last_error = str(e)
            self.consecutive_errors += 1
            raise
        co2_ppm = await self._read_co2()

        row = ReadingRow(
            ts=datetime.now(timezone.utc),
            sensor_id=self.settings.sensor_id,
            temperature_c=reading.temperature_c,
            pressure_hpa=reading.pressure_hpa,
            humidity_pct=reading.humidity_pct,
            co2_ppm=co2_ppm,
        )
        async with self.sessions() as session:
            session.add(row)
            await session.commit()

        self.last_ok_at = row.ts
        self.last_error = None
        self.consecutive_errors = 0
        return row

    async def prune(self) -> int:
        if self.settings.retention_days <= 0:
            return 0
        cutoff = datetime.now(timezone.utc) - timedelta(days=self.settings.retention_days)
        async with self.sessions() as session:
            result = await session.execute(delete(ReadingRow).where(ReadingRow.ts < cutoff))
            await session.commit()
        return result.rowcount or 0

    async def run(self) -> None:
        interval = self.settings.sample_interval_s
        log.info(
            "sampler started: driver=%s co2=%s interval=%.1fs",
            self.sensor.name, self.co2.name if self.co2 else "none", interval,
        )
        next_tick = time.monotonic()
        while True:
            try:
                await self.sample_once()
            except SensorError as e:
                # Log the first failure and then every 30th, so a disconnected
                # sensor doesn't flood the journal.
                if self.consecutive_errors == 1 or self.consecutive_errors % 30 == 0:
                    log.warning("sensor read failed (%d in a row): %s", self.consecutive_errors, e)
            except Exception:
                log.exception("failed to store reading")

            if time.monotonic() - self._last_prune > PRUNE_EVERY_S:
                self._last_prune = time.monotonic()
                try:
                    if removed := await self.prune():
                        log.info("pruned %d readings older than %d days", removed, self.settings.retention_days)
                except Exception:
                    log.exception("retention prune failed")

            next_tick += interval
            delay = next_tick - time.monotonic()
            if delay < 0:  # fell behind (e.g. DB outage); don't burst to catch up
                next_tick = time.monotonic()
                delay = 0
            await asyncio.sleep(delay)

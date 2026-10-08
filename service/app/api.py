import csv
import io
from datetime import datetime, timedelta, timezone

from fastapi import APIRouter, HTTPException, Query, Request
from fastapi.responses import StreamingResponse
from sqlalchemy import func, literal_column, select, text

from .db import ReadingRow
from .driver import SensorError
from .levels import CO2_LEVELS
from .schemas import Co2Health, Co2LevelOut, FieldStats, Health, ReadingOut, SensorHealth, Series, SeriesPoint, Stats

router = APIRouter(prefix="/api")

FIELDS = ("temperature_c", "pressure_hpa", "humidity_pct", "co2_ppm")
MAX_HOURS = 24 * 365


def _window(hours: float) -> tuple[datetime, datetime]:
    end = datetime.now(timezone.utc)
    return end - timedelta(hours=hours), end


def _round(v) -> float | None:
    return None if v is None else round(float(v), 2)


def _sensor(request: Request, sensor_id: str | None) -> str:
    return sensor_id or request.app.state.settings.sensor_id


@router.get("/health", response_model=Health)
async def health(request: Request):
    state = request.app.state
    try:
        async with state.sessions() as session:
            await session.execute(text("SELECT 1"))
        db_ok = True
    except Exception:
        db_ok = False

    sampler = state.sampler
    task = state.sampler_task
    chip = sampler.sensor.chip_id()
    sensor_ok = sampler.last_error is None and sampler.co2_last_error is None
    co2 = None
    if sampler.co2 is not None:
        co2 = Co2Health(
            driver=sampler.co2.name,
            last_ok_at=sampler.co2_last_ok_at,
            last_error=sampler.co2_last_error,
            consecutive_errors=sampler.co2_consecutive_errors,
            level=sampler.co2_detector.level,
            level_since=sampler.co2_detector.since,
        )
    return Health(
        status="ok" if db_ok and sensor_ok else "degraded",
        database=db_ok,
        sensor=SensorHealth(
            driver=sampler.sensor.name,
            chip_id=f"0x{chip:02X}" if chip else None,
            sampler_running=task is not None and not task.done(),
            last_ok_at=sampler.last_ok_at,
            last_error=sampler.last_error,
            consecutive_errors=sampler.consecutive_errors,
        ),
        co2=co2,
        sample_interval_s=state.settings.sample_interval_s,
    )


@router.get("/levels/co2", response_model=list[Co2LevelOut])
async def co2_levels():
    """CO2 danger bands, lowest first. A band runs from its min_ppm up to the next one's."""
    return CO2_LEVELS


@router.get("/readings/latest", response_model=ReadingOut)
async def latest(request: Request, sensor_id: str | None = None):
    async with request.app.state.sessions() as session:
        row = await session.scalar(
            select(ReadingRow)
            .where(ReadingRow.sensor_id == _sensor(request, sensor_id))
            .order_by(ReadingRow.ts.desc())
            .limit(1)
        )
    if row is None:
        raise HTTPException(404, "no readings yet")
    return row


@router.get("/readings", response_model=list[ReadingOut])
async def list_readings(
    request: Request,
    start: datetime | None = None,
    end: datetime | None = None,
    limit: int = Query(500, ge=1, le=10_000),
    sensor_id: str | None = None,
):
    """Raw readings, newest first."""
    q = select(ReadingRow).where(ReadingRow.sensor_id == _sensor(request, sensor_id))
    if start:
        q = q.where(ReadingRow.ts >= start)
    if end:
        q = q.where(ReadingRow.ts < end)
    async with request.app.state.sessions() as session:
        rows = await session.scalars(q.order_by(ReadingRow.ts.desc()).limit(limit))
        return list(rows)


@router.post("/readings/sample", response_model=ReadingOut, status_code=201)
async def sample_now(request: Request):
    """Read the sensor immediately and store the result."""
    try:
        return await request.app.state.sampler.sample_once()
    except SensorError as e:
        raise HTTPException(503, str(e))


@router.get("/readings/series", response_model=Series)
async def series(
    request: Request,
    hours: float = Query(24, gt=0, le=MAX_HOURS),
    points: int = Query(300, ge=10, le=5000),
    sensor_id: str | None = None,
):
    """Time-bucketed averages, sized so the chart gets at most `points` points."""
    sid = _sensor(request, sensor_id)
    start, end = _window(hours)
    bucket_s = max(int(request.app.state.settings.sample_interval_s), int(hours * 3600 / points), 1)

    # bucket_s is a server-computed int, so inlining it is safe and avoids
    # asyncpg failing to infer a parameter type inside the arithmetic.
    bucket = (
        func.floor(func.extract("epoch", ReadingRow.ts) / literal_column(str(bucket_s)))
        * literal_column(str(bucket_s))
    ).label("bucket")
    q = (
        select(bucket, *(func.avg(getattr(ReadingRow, f)) for f in FIELDS))
        .where(ReadingRow.sensor_id == sid, ReadingRow.ts >= start)
        .group_by(bucket)
        .order_by(bucket)
    )
    async with request.app.state.sessions() as session:
        rows = (await session.execute(q)).all()

    return Series(
        sensor_id=sid,
        start=start,
        end=end,
        bucket_s=bucket_s,
        points=[
            SeriesPoint(
                ts=datetime.fromtimestamp(float(b), timezone.utc),
                **{f: _round(v) for f, v in zip(FIELDS, vals)},
            )
            for b, *vals in rows
        ],
    )


@router.get("/readings/stats", response_model=Stats)
async def stats(
    request: Request,
    hours: float = Query(24, gt=0, le=MAX_HOURS),
    sensor_id: str | None = None,
):
    sid = _sensor(request, sensor_id)
    start, end = _window(hours)
    cols = [func.count()]
    for f in FIELDS:
        c = getattr(ReadingRow, f)
        cols += [func.min(c), func.max(c), func.avg(c)]
    q = select(*cols).where(ReadingRow.sensor_id == sid, ReadingRow.ts >= start)
    async with request.app.state.sessions() as session:
        row = (await session.execute(q)).one()

    def field(i: int) -> FieldStats:
        lo, hi, avg = row[1 + 3 * i : 4 + 3 * i]
        return FieldStats(min=_round(lo), max=_round(hi), avg=_round(avg))

    return Stats(
        sensor_id=sid,
        start=start,
        end=end,
        count=row[0],
        **{f: field(i) for i, f in enumerate(FIELDS)},
    )


@router.get("/readings.csv")
async def export_csv(
    request: Request,
    hours: float = Query(24, gt=0, le=MAX_HOURS),
    sensor_id: str | None = None,
):
    sid = _sensor(request, sensor_id)
    start, _ = _window(hours)
    sessions = request.app.state.sessions

    async def rows():
        buf = io.StringIO()
        writer = csv.writer(buf)
        writer.writerow(["ts", "sensor_id", *FIELDS])
        async with sessions() as session:
            result = await session.stream_scalars(
                select(ReadingRow)
                .where(ReadingRow.sensor_id == sid, ReadingRow.ts >= start)
                .order_by(ReadingRow.ts)
                .execution_options(yield_per=1000)
            )
            async for r in result:
                writer.writerow([r.ts.isoformat(), r.sensor_id, *(getattr(r, f) for f in FIELDS)])
                if buf.tell() > 64_000:
                    yield buf.getvalue()
                    buf.seek(0)
                    buf.truncate()
        yield buf.getvalue()

    filename = f"{sid}-{start:%Y%m%d%H%M}.csv"
    return StreamingResponse(
        rows(), media_type="text/csv", headers={"Content-Disposition": f'attachment; filename="{filename}"'}
    )

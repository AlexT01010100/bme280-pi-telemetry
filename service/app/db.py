from datetime import datetime

from sqlalchemy import DateTime, Float, Index, String, func
from sqlalchemy.ext.asyncio import AsyncEngine, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class ReadingRow(Base):
    __tablename__ = "readings"

    id: Mapped[int] = mapped_column(primary_key=True)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    sensor_id: Mapped[str] = mapped_column(String(64))
    temperature_c: Mapped[float] = mapped_column(Float)
    pressure_hpa: Mapped[float] = mapped_column(Float)
    humidity_pct: Mapped[float] = mapped_column(Float)

    __table_args__ = (Index("ix_readings_sensor_ts", "sensor_id", "ts"),)


def make_engine(url: str) -> AsyncEngine:
    return create_async_engine(url, pool_pre_ping=True)


def make_sessionmaker(engine: AsyncEngine) -> async_sessionmaker:
    return async_sessionmaker(engine, expire_on_commit=False)


async def create_schema(engine: AsyncEngine) -> None:
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)

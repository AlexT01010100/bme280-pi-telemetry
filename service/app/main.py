import asyncio
import contextlib
import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .api import router
from .config import Settings, get_settings
from .db import create_schema, make_engine, make_sessionmaker
from .driver import create_co2_sensor, create_sensor
from .sampler import Sampler

STATIC_DIR = Path(__file__).parent / "static"

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or get_settings()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        engine = make_engine(settings.database_url)
        await create_schema(engine)
        sessions = make_sessionmaker(engine)
        sensor = create_sensor(settings)
        co2 = create_co2_sensor(settings)

        app.state.settings = settings
        app.state.sessions = sessions
        app.state.sampler = Sampler(sensor, sessions, settings, co2)
        app.state.sampler_task = (
            asyncio.create_task(app.state.sampler.run()) if settings.sampler_enabled else None
        )
        try:
            yield
        finally:
            if task := app.state.sampler_task:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await task
            sensor.close()
            if co2:
                co2.close()
            await engine.dispose()

    app = FastAPI(title="Indoor Air Monitor", version="1.0.0", lifespan=lifespan)
    app.include_router(router)
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    @app.get("/", include_in_schema=False)
    async def dashboard():
        return FileResponse(STATIC_DIR / "index.html")

    return app


app = create_app()

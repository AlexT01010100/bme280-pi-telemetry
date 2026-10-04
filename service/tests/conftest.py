import os

import pytest
from asgi_lifespan import LifespanManager
from httpx import ASGITransport, AsyncClient
from sqlalchemy import text

from app.config import Settings
from app.main import create_app

TEST_DB = os.environ.get("TEST_DATABASE_URL")


@pytest.fixture
def settings():
    if not TEST_DB:
        pytest.skip("TEST_DATABASE_URL not set")
    return Settings(database_url=TEST_DB, driver="mock", sampler_enabled=False, sample_interval_s=10)


@pytest.fixture
async def app(settings):
    app = create_app(settings)
    async with LifespanManager(app):
        async with app.state.sessions() as s:
            await s.execute(text("TRUNCATE readings"))
            await s.commit()
        yield app


@pytest.fixture
async def client(app):
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        yield c

"""Exercises the real ctypes bindings against libbme280.so (Linux only).

Without SPI hardware, bme280_init fails to open the device; that still proves
the library loads, the symbols/signatures match, and errors map to SensorError.
On a Pi with the sensor wired, set BME_TEST_SPI_DEVICE to read it for real.
"""
import os
import sys

import pytest

from app.config import REPO_ROOT
from app.driver import NativeBME280, SensorError

LIB = REPO_ROOT / "libbme280.so"
pytestmark = pytest.mark.skipif(not (sys.platform.startswith("linux") and LIB.exists()),
                                reason="needs libbme280.so on Linux (run `make`)")


def test_missing_device_raises_sensor_error():
    sensor = NativeBME280(LIB, "/dev/does-not-exist", 500_000)
    with pytest.raises(SensorError, match="could not open SPI device: No such file"):
        sensor.read()
    assert sensor.chip_id() is None
    sensor.close()


@pytest.mark.skipif(not os.environ.get("BME_TEST_SPI_DEVICE"), reason="no sensor attached")
def test_real_sensor_read():
    sensor = NativeBME280(LIB, os.environ["BME_TEST_SPI_DEVICE"], 500_000)
    r = sensor.read()
    assert sensor.chip_id() == 0x60
    assert 300 <= r.pressure_hpa <= 1100
    sensor.close()

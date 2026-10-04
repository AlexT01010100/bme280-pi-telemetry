"""Python bindings for the C BME280 driver (libbme280.so) plus a mock for development."""

from __future__ import annotations

import ctypes
import math
import os
import random
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

# Mirrors the return codes in bme280.h
_ERRORS = {
    -1: "could not open SPI device",
    -2: "SPI transfer failed",
    -3: "unexpected chip ID (expected 0x60) - check wiring",
    -4: "failed to read calibration data",
    -5: "driver not initialised",
}

# BME280 datasheet operating range. Values outside it almost always mean a bad
# SPI read (loose jumper, floating MISO) rather than a real measurement.
_VALID_RANGES = {
    "temperature_c": (-40.0, 85.0),
    "pressure_hpa": (300.0, 1100.0),
    "humidity_pct": (0.0, 100.0),
}


class SensorError(RuntimeError):
    pass


@dataclass(frozen=True)
class Reading:
    temperature_c: float
    pressure_hpa: float
    humidity_pct: float

    def validate(self) -> Reading:
        for field, (lo, hi) in _VALID_RANGES.items():
            value = getattr(self, field)
            if not (lo <= value <= hi) or math.isnan(value):
                raise SensorError(f"{field}={value:.2f} outside sensor range [{lo}, {hi}]")
        return self


class Sensor(Protocol):
    name: str

    def read(self) -> Reading: ...
    def chip_id(self) -> int | None: ...
    def close(self) -> None: ...


class _CReading(ctypes.Structure):
    _fields_ = [
        ("temperature_c", ctypes.c_double),
        ("pressure_hpa", ctypes.c_double),
        ("humidity_pct", ctypes.c_double),
    ]


class NativeBME280:
    """Thread-safe wrapper around libbme280.so.

    The C driver keeps its state in globals, so every call is serialised through
    one lock. After any failure the device is closed and re-initialised on the
    next read, which recovers from a sensor that was unplugged and reconnected.
    """

    name = "native"

    def __init__(self, lib_path: Path, device: str, speed_hz: int):
        if not lib_path.exists():
            raise SensorError(f"{lib_path} not found - run `make` in the repo root on the Pi")
        self._lib = ctypes.CDLL(str(lib_path), use_errno=True)
        self._lib.bme280_init.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
        self._lib.bme280_init.restype = ctypes.c_int
        self._lib.bme280_read.argtypes = [ctypes.POINTER(_CReading)]
        self._lib.bme280_read.restype = ctypes.c_int
        self._lib.bme280_chip_id.argtypes = []
        self._lib.bme280_chip_id.restype = ctypes.c_uint8
        self._lib.bme280_close.argtypes = []
        self._lib.bme280_close.restype = None

        self._device = device
        self._speed_hz = speed_hz
        self._lock = threading.Lock()
        self._initialised = False

    def _check(self, rc: int, action: str) -> None:
        if rc == 0:
            return
        msg = _ERRORS.get(rc, f"error code {rc}")
        errno = ctypes.get_errno()
        if errno and rc in (-1, -2, -4):
            msg += f": {os.strerror(errno)}"
        raise SensorError(f"{action} {self._device}: {msg}")

    def _ensure_init(self) -> None:
        if not self._initialised:
            self._check(self._lib.bme280_init(self._device.encode(), self._speed_hz), "init")
            self._initialised = True

    def read(self) -> Reading:
        with self._lock:
            try:
                self._ensure_init()
                raw = _CReading()
                self._check(self._lib.bme280_read(ctypes.byref(raw)), "read")
                return Reading(raw.temperature_c, raw.pressure_hpa, raw.humidity_pct).validate()
            except SensorError:
                self._lib.bme280_close()
                self._initialised = False
                raise

    def chip_id(self) -> int | None:
        with self._lock:
            return self._lib.bme280_chip_id() or None

    def close(self) -> None:
        with self._lock:
            self._lib.bme280_close()
            self._initialised = False


class MockBME280:
    """Plausible indoor readings with a slow daily cycle and a little noise."""

    name = "mock"

    def read(self) -> Reading:
        day = 2 * math.pi * (time.time() % 86400) / 86400
        return Reading(
            temperature_c=round(21.5 + 2.0 * math.sin(day) + random.gauss(0, 0.05), 2),
            pressure_hpa=round(1013.2 + 3.0 * math.sin(day / 2) + random.gauss(0, 0.08), 2),
            humidity_pct=round(45.0 - 6.0 * math.sin(day) + random.gauss(0, 0.3), 2),
        )

    def chip_id(self) -> int | None:
        return 0x60

    def close(self) -> None:
        pass


def create_sensor(settings) -> Sensor:
    if settings.driver == "mock":
        return MockBME280()
    return NativeBME280(settings.lib_path, settings.spi_device, settings.spi_speed_hz)

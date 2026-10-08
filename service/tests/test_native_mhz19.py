"""Exercises the ctypes bindings to libmhz19.so (Linux only).

A pseudo-terminal stands in for the UART, with a thread playing the sensor, so
the whole path - Python -> ctypes -> termios/poll in C -> bytes on a tty - runs
without hardware. On a Pi with the sensor wired, set BME_TEST_CO2_DEVICE to
read it for real.
"""
import os
import sys
import threading

import pytest

from app.config import REPO_ROOT
from app.driver import NativeMHZ19, SensorError

LIB = REPO_ROOT / "libmhz19.so"
pytestmark = pytest.mark.skipif(not (sys.platform.startswith("linux") and LIB.exists()),
                                reason="needs libmhz19.so on Linux (run `make`)")

READ_CMD = bytes([0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79])
GOOD = bytes([0xFF, 0x86, 0x05, 0xB5, 0x40, 0x00, 0x00, 0x00, 0x80])  # 1461 ppm


class FakeSensor:
    """Answers each read command on the pty master with the next queued reply (None = stay silent)."""

    def __init__(self, replies):
        # Keep our slave fd open for the whole test: once every slave fd is
        # closed, reads on the master fail with EIO until it is reopened.
        self.master, self._slave = os.openpty()
        self.device = os.ttyname(self._slave)
        self.replies = list(replies)
        self.commands = []
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        while self.replies:
            buf = b""
            while len(buf) < 9:
                try:
                    chunk = os.read(self.master, 9 - len(buf))
                except OSError:  # test finished and closed the pty
                    return
                if not chunk:
                    return
                buf += chunk
            self.commands.append(buf)
            reply = self.replies.pop(0)
            if reply:
                os.write(self.master, reply)

    def close(self):
        os.close(self._slave)
        os.close(self.master)


def test_reads_ppm_over_tty():
    fake = FakeSensor([GOOD])
    sensor = NativeMHZ19(LIB, fake.device, timeout_ms=500)
    try:
        assert sensor.read() == 1461
        assert fake.commands == [READ_CMD]
    finally:
        sensor.close()
        fake.close()


def test_silent_sensor_times_out_then_recovers():
    fake = FakeSensor([None, GOOD])
    sensor = NativeMHZ19(LIB, fake.device, timeout_ms=200)
    try:
        with pytest.raises(SensorError, match="no response - check TX/RX wiring"):
            sensor.read()
        assert sensor.read() == 1461  # port was closed and reopened after the failure
    finally:
        sensor.close()
        fake.close()


def test_bad_checksum_raises():
    bad = GOOD[:3] + bytes([GOOD[3] ^ 0x10]) + GOOD[4:]
    fake = FakeSensor([bad])
    sensor = NativeMHZ19(LIB, fake.device, timeout_ms=500)
    try:
        with pytest.raises(SensorError, match="bad checksum"):
            sensor.read()
    finally:
        sensor.close()
        fake.close()


def test_missing_device_raises_sensor_error():
    sensor = NativeMHZ19(LIB, "/dev/does-not-exist")
    with pytest.raises(SensorError, match="could not open serial device: No such file"):
        sensor.read()
    sensor.close()


@pytest.mark.skipif(not os.environ.get("BME_TEST_CO2_DEVICE"), reason="no CO2 sensor attached")
def test_real_sensor_read():
    sensor = NativeMHZ19(LIB, os.environ["BME_TEST_CO2_DEVICE"])
    assert 0 <= sensor.read() <= 10_000
    sensor.close()

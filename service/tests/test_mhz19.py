"""MH-Z19 frame decoding and serial handling, without hardware."""
import pytest

from app import driver
from app.driver import MHZ19, SensorError, mhz19_checksum, parse_mhz19

# A real response: 0x05B5 = 1461 ppm
GOOD = bytes([0xFF, 0x86, 0x05, 0xB5, 0x40, 0x00, 0x00, 0x00, 0x80])


def test_checksum_matches_read_command():
    assert mhz19_checksum(driver._MHZ19_READ_CMD) == 0x79


def test_parse_good_frame():
    assert parse_mhz19(GOOD) == 1461


@pytest.mark.parametrize(
    "frame, match",
    [
        (b"", "got 0/9 bytes"),
        (GOOD[:5], "got 5/9 bytes"),
        (GOOD[:8] + b"\x00", "bad checksum"),
        (bytes([0xFF, 0x99]) + GOOD[2:], "unexpected response"),
    ],
)
def test_parse_rejects_bad_frames(frame, match):
    with pytest.raises(SensorError, match=match):
        parse_mhz19(frame)


def test_parse_rejects_out_of_range():
    frame = bytearray([0xFF, 0x86, 0xFF, 0xFF, 0, 0, 0, 0, 0])
    frame[8] = mhz19_checksum(frame)
    with pytest.raises(SensorError, match="outside sensor range"):
        parse_mhz19(bytes(frame))


class FakePort:
    def __init__(self, response: bytes):
        self.response = response
        self.written = b""
        self.closed = False

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written += data

    def read(self, n):
        return self.response[:n]

    def close(self):
        self.closed = True


def test_read_sends_command_and_reopens_after_failure(monkeypatch):
    ports = [FakePort(b""), FakePort(GOOD)]
    monkeypatch.setattr(driver.serial, "Serial", lambda *a, **kw: ports.pop(0))
    sensor = MHZ19("/dev/ttyFAKE")

    with pytest.raises(SensorError, match="/dev/ttyFAKE: got 0/9 bytes"):
        sensor.read()
    assert sensor.read() == 1461  # a fresh port was opened for the retry
    assert not ports


def test_missing_device_raises_sensor_error():
    with pytest.raises(SensorError, match="/dev/does-not-exist"):
        MHZ19("/dev/does-not-exist").read()

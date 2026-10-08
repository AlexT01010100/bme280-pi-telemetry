from datetime import datetime, timedelta, timezone

import pytest

from app.levels import CO2_LEVELS, HYSTERESIS_PPM, Co2Detector, classify_co2

T0 = datetime(2026, 10, 7, 12, 0, tzinfo=timezone.utc)


@pytest.mark.parametrize(
    "ppm, key",
    [(420, "good"), (799, "good"), (800, "moderate"), (1199, "moderate"), (1200, "poor"),
     (1999, "poor"), (2000, "unhealthy"), (4999, "unhealthy"), (5000, "dangerous"), (10_000, "dangerous")],
)
def test_classify_boundaries(ppm, key):
    assert classify_co2(ppm).key == key


def test_levels_are_ordered_and_only_high_ones_alert():
    mins = [lv.min_ppm for lv in CO2_LEVELS]
    assert mins == sorted(mins) and mins[0] == 0
    assert [lv.key for lv in CO2_LEVELS if lv.alert] == ["unhealthy", "dangerous"]


def test_detector_reports_changes_and_since():
    d = Co2Detector()
    assert d.update(600, T0) == (None, classify_co2(600))
    assert d.update(650, T0 + timedelta(minutes=1)) is None        # same level: no event
    assert d.since == T0

    prev, new = d.update(2100, T0 + timedelta(minutes=2))          # rises immediately, can skip levels
    assert (prev.key, new.key) == ("good", "unhealthy")
    assert d.since == T0 + timedelta(minutes=2)


def test_detector_hysteresis_on_the_way_down():
    d = Co2Detector()
    d.update(1250, T0)
    assert d.level.key == "poor"
    # Hovering just under the boundary keeps the level...
    for ppm in (1199, 1180, 1200 - HYSTERESIS_PPM):
        assert d.update(ppm, T0) is None and d.level.key == "poor"
    # ...until it clearly drops below it.
    prev, new = d.update(1200 - HYSTERESIS_PPM - 1, T0)
    assert (prev.key, new.key) == ("poor", "moderate")


def test_detector_large_drop_skips_levels():
    d = Co2Detector()
    d.update(5200, T0)
    _, new = d.update(500, T0)
    assert new.key == "good"

"""CO2 danger levels and a detector that tracks the current one.

Bands follow common indoor-air guidance: outdoor air is about 420 ppm, studies
report drowsiness and poorer concentration from roughly 1000-1200 ppm,
headaches become common around 2000 ppm, and 5000 ppm is the OSHA 8-hour
workplace exposure limit.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime


@dataclass(frozen=True)
class Co2Level:
    key: str
    label: str
    min_ppm: int
    advice: str
    alert: bool  # worth interrupting someone for


CO2_LEVELS = (
    Co2Level("good", "Good", 0, "Fresh air.", False),
    Co2Level("moderate", "Moderate", 800, "Fine for now. Air the room out if it keeps rising.", False),
    Co2Level("poor", "Poor", 1200, "Stuffy. Concentration may suffer, so open a window.", False),
    Co2Level("unhealthy", "Unhealthy", 2000, "Headaches and drowsiness are likely. Ventilate now.", True),
    Co2Level("dangerous", "Dangerous", 5000, "Above the OSHA 8-hour exposure limit. Get to fresh air.", True),
)

# How far below a boundary CO2 must fall before the level steps down. Without
# it, a reading hovering at 1199/1201 ppm would flip the level every sample.
HYSTERESIS_PPM = 50


def classify_co2(ppm: float) -> Co2Level:
    """Plain threshold lookup, no memory."""
    level = CO2_LEVELS[0]
    for candidate in CO2_LEVELS:
        if ppm >= candidate.min_ppm:
            level = candidate
    return level


class Co2Detector:
    """Tracks the current level. Rises immediately; falls only once CO2 is
    HYSTERESIS_PPM below the current level's lower bound."""

    def __init__(self) -> None:
        self.level: Co2Level | None = None
        self.since: datetime | None = None

    def update(self, ppm: float, now: datetime) -> tuple[Co2Level | None, Co2Level] | None:
        """Feed a reading. Returns (previous, new) when the level changed, else None."""
        level = classify_co2(ppm)
        current = self.level
        if current is not None and level.min_ppm < current.min_ppm and ppm >= current.min_ppm - HYSTERESIS_PPM:
            level = current
        if level is current:
            return None
        self.level, self.since = level, now
        return current, level

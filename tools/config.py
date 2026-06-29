from __future__ import annotations
from dataclasses import dataclass, field
from typing import Optional
import math
import yaml


def _num(v, default=None):
    if v is None:
        return default
    if isinstance(v, (int, float)):
        return float(v)
    return float(eval(str(v).replace('^', '**'), {'__builtins__': {}}, vars(math)))


@dataclass
class Channel:
    channel:       str              # hardware name in DEVICE_INFO (e.g. "ADC_IN0")
    name:          str              # display label
    scale:         float            = 1.0
    offset:        float            = 0.0
    unit:          str              = 'raw'
    y_min:         Optional[float]  = None
    y_max:         Optional[float]  = None
    sampling_time: int              = 0  # ADC SMPR enum 0-7 → 3/15/28/56/84/112/144/480 cycles

    def convert(self, raw: float) -> float:
        return raw * self.scale + self.offset


@dataclass
class Config:
    # Insertion order is preserved; the monitor lays out plot rows in this order.
    boards: dict[str, list[Channel]] = field(default_factory=dict)
    log:    Optional[str]            = None
    window: float                    = 5.0  # seconds of history shown in the plot


def _parse_channels(ch_list: list, where: str) -> list[Channel]:
    channels = [
        Channel(
            channel=str(ch['channel']),
            name=str(ch.get('name', ch['channel'])),
            scale=_num(ch.get('scale', 1.0)),
            offset=_num(ch.get('offset', 0.0)),
            unit=str(ch.get('unit', 'raw')),
            y_min=_num(ch.get('y_min')),
            y_max=_num(ch.get('y_max')),
            sampling_time=int(ch.get('sampling_time', 0)),
        )
        for ch in ch_list
    ]
    for ch in channels:
        if not 0 <= ch.sampling_time <= 7:
            raise ValueError(
                f"{where}: channel '{ch.name}' sampling_time {ch.sampling_time} "
                f"out of range (must be 0–7; maps to 3/15/28/56/84/112/144/480 ADC cycles)"
            )
    seen: set[str] = set()
    for ch in channels:
        if ch.name in seen:
            raise ValueError(f"{where}: duplicate channel name '{ch.name}'")
        seen.add(ch.name)
    return channels


def load(path: str) -> Config:
    with open(path) as f:
        data = yaml.safe_load(f) or {}
    raw_boards = data.get('boards') or {}
    if not isinstance(raw_boards, dict):
        raise ValueError("'boards' must be a mapping of USB serial → {channels: [...]}")
    boards: dict[str, list[Channel]] = {}
    for serial, entry in raw_boards.items():
        # Normalize: serials are case-insensitive ASCII; CubeMX produces uppercase hex.
        key = str(serial).upper()
        if key in boards:
            raise ValueError(f"duplicate board serial '{key}'")
        ch_list = (entry or {}).get('channels', [])
        boards[key] = _parse_channels(ch_list, f"board {key}")
    return Config(
        boards=boards,
        log=data.get('log'),
        window=float(data.get('window', 5.0)),
    )

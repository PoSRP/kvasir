from __future__ import annotations
from dataclasses import dataclass, field
from typing import Optional
import math
import yaml

from .session import Channel

NUM_CH = 10  # kvasir board has ADC_IN0..ADC_IN9


def _num(v, default=None):
    if v is None:
        return default
    if isinstance(v, (int, float)):
        return float(v)
    return float(eval(str(v).replace('^', '**'), {'__builtins__': {}}, vars(math)))


def _channel_name_to_id(name: str) -> int:
    prefix = 'ADC_IN'
    if name.startswith(prefix):
        rest = name[len(prefix):]
        if rest.isdigit():
            n = int(rest)
            if 0 <= n < NUM_CH:
                return n
    raise ValueError(f"channel '{name}' must be one of ADC_IN0..ADC_IN{NUM_CH - 1}")


@dataclass
class Config:
    # Insertion order is preserved; the monitor lays out plot rows in this order.
    boards: dict[str, list[Channel]] = field(default_factory=dict)
    log:    Optional[str]            = None
    window: float                    = 5.0  # seconds of history shown in the plot


def _parse_channels(ch_list: list, where: str) -> list[Channel]:
    channels = [
        Channel(
            channel_id=_channel_name_to_id(str(ch['channel'])),
            name=str(ch.get('name', ch['channel'])),
            unit=str(ch.get('unit', 'V')),
            y_min=_num(ch.get('y_min')),
            y_max=_num(ch.get('y_max')),
            sampling_time=int(ch.get('sampling_time', 0)),
            formula=(str(ch['formula']) if ch.get('formula') else None),
        )
        for ch in ch_list
    ]
    for ch in channels:
        if not 0 <= ch.sampling_time <= 7:
            raise ValueError(
                f"{where}: channel '{ch.name}' sampling_time {ch.sampling_time} "
                f"out of range (must be 0-7; maps to 3/15/28/56/84/112/144/480 ADC cycles)"
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
        raise ValueError("'boards' must be a mapping of USB serial -> {channels: [...]}")
    boards: dict[str, list[Channel]] = {}
    for serial, entry in raw_boards.items():
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

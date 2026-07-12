from __future__ import annotations
from dataclasses import dataclass, field
from typing import Optional
import ast
import struct
import time
import numpy as np

from .frame_parser import (
    FrameParser, AnyFrame,
    StreamDataFrame, AckFrame,
)
from .protocol import MAGIC, FrameType


ADC_CLK_HZ = 24_000_000
_SMPR_CYCLES = (3, 15, 28, 56, 84, 112, 144, 480)
ADC_VREF     = 3.3
ADC_MAX_CODE = 4095


def compute_sample_rate(sampling_times: list[int]) -> int:
    """Per-channel sample rate when the ADC scans the given channels in series.

    All channels share the same rate: total scan cycles = sum over channels of
    (sampling + 12 conversion) cycles; per-channel rate = ADC_CLK / total.
    Returns 0 when no channels are enabled."""
    if not sampling_times:
        return 0
    total = sum(_SMPR_CYCLES[s] + 12 for s in sampling_times)
    return ADC_CLK_HZ // total


_FORMULA_NAMES: dict[str, object] = {
    n: getattr(np, n) for n in (
        'log', 'log10', 'log2', 'exp', 'sqrt', 'sin', 'cos', 'tan',
        'arcsin', 'arccos', 'arctan', 'arctan2', 'abs', 'sign',
        'floor', 'ceil', 'clip', 'power', 'minimum', 'maximum',
        'pi', 'e',
    )
}


def _compile_formula(channel_name: str, source: str):
    """Parse+compile the formula. Rejects unknown names at load time so a bad
    expression fails on config load, not mid-stream."""
    try:
        tree = ast.parse(source, mode='eval')
    except SyntaxError as e:
        raise ValueError(f"channel '{channel_name}': invalid formula: {e}") from e
    used    = {n.id for n in ast.walk(tree) if isinstance(n, ast.Name)}
    unknown = used - _FORMULA_NAMES.keys() - {'v'}
    if unknown:
        raise ValueError(
            f"channel '{channel_name}': formula uses unknown names "
            f"{sorted(unknown)}. Available: v, {', '.join(sorted(_FORMULA_NAMES))}"
        )
    return compile(tree, f'<formula:{channel_name}>', 'eval')


@dataclass
class Channel:
    channel_id:    int
    name:          str
    unit:          str              = 'V'
    y_min:         Optional[float]  = None
    y_max:         Optional[float]  = None
    sampling_time: int              = 0
    formula:       Optional[str]    = None
    _code:         object           = field(default=None, init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        if self.formula is not None:
            self._code = _compile_formula(self.name, self.formula)

    def convert(self, raw):
        v = raw * (ADC_VREF / ADC_MAX_CODE)
        if self._code is None:
            return v
        return eval(self._code, {'__builtins__': {}}, {'v': v, **_FORMULA_NAMES})


def _make_frame(type_byte: int, payload: bytes) -> bytes:
    return bytes([MAGIC, type_byte, len(payload) >> 8, len(payload) & 0xFF]) + payload


class Session:
    """Manages the connect -> CONFIG -> START -> stream -> STOP lifecycle.

    port: any object with read(n)->bytes, write(bytes), and in_waiting property.
    Accepts a real serial.Serial or a FakePort for testing.
    """

    def __init__(self, port, channels: list[Channel]) -> None:
        self._port     = port
        self._channels = channels
        self._parser   = FrameParser()
        self._pending: list[AnyFrame] = []

    def connect(self, timeout: float = 5.0) -> None:
        """STOP -> ACK -> CONFIG -> ACK -> START -> ACK."""
        deadline = time.monotonic() + timeout
        # STOP forces the firmware into UNCONFIGURED regardless of prior state.
        self._port.write(_make_frame(FrameType.STOP, b""))
        self._wait_for_ack(FrameType.STOP, deadline)

        self._port.write(_make_frame(FrameType.CONFIG, self._build_config_payload()))
        self._wait_for_ack(FrameType.CONFIG, deadline)

        self._port.write(_make_frame(FrameType.START, b""))
        self._wait_for_ack(FrameType.START, deadline)

    def feed(self, data: bytes) -> list[StreamDataFrame]:
        """Parse bytes from the device, return only stream data frames."""
        return [f for f in self._parser.feed(data) if isinstance(f, StreamDataFrame)]

    def stop(self) -> None:
        self._port.write(_make_frame(FrameType.STOP, b""))

    def close(self) -> None:
        if hasattr(self._port, 'close'):
            self._port.close()

    def _recv_frame(self, deadline: float | None) -> AnyFrame:
        while True:
            if self._pending:
                return self._pending.pop(0)
            if deadline is not None and time.monotonic() >= deadline:
                raise TimeoutError('session connect timed out')
            n = self._port.in_waiting or 1
            data = self._port.read(n)
            if data:
                self._pending.extend(self._parser.feed(data))

    def _wait_for_ack(self, cmd_type: int, deadline: float | None) -> AckFrame:
        while True:
            f = self._recv_frame(deadline)
            if isinstance(f, AckFrame) and f.cmd_type == int(cmd_type):
                return f

    def _build_config_payload(self) -> bytes:
        payload = b""
        for ch in self._channels:
            cfg = struct.pack(">BB", 1, ch.sampling_time)
            payload += bytes([ch.channel_id, len(cfg)]) + cfg
        return payload

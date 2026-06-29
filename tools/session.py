from __future__ import annotations
import struct
import time

from .frame_parser import (
    FrameParser, AnyFrame,
    DeviceInfoFrame, StreamDataFrame, AckFrame,
)
from .protocol import MAGIC, FrameType, ChannelType
from .config import Channel


def _make_frame(type_byte: int, payload: bytes) -> bytes:
    return bytes([MAGIC, type_byte, len(payload) >> 8, len(payload) & 0xFF]) + payload


class Session:
    """Manages the connect → CONFIG → START → stream → STOP lifecycle.

    port: any object with read(n)->bytes, write(bytes), and in_waiting property.
    Accepts a real serial.Serial or a FakePort for testing.
    """

    def __init__(self, port, channels: list[Channel]) -> None:
        self._port     = port
        self._channels = channels
        self._parser   = FrameParser()
        self._pending: list[AnyFrame] = []
        self.device_info:  DeviceInfoFrame | None = None
        self.actual_rates: dict[int, int]         = {}  # channel_id → actual sample rate

    def connect(self, timeout: float = 5.0) -> None:
        """STOP (to force UNCONFIGURED) → DEVICE_INFO → CONFIG → ACK → START → ACK."""
        deadline = time.monotonic() + timeout
        # Firmware re-emits DEVICE_INFO after handling STOP regardless of prior state.
        self._port.write(_make_frame(FrameType.STOP, b""))
        self.device_info = self._wait_for_type(DeviceInfoFrame, deadline)

        self._port.write(_make_frame(FrameType.CONFIG, self._build_config_payload()))
        cfg_ack = self._wait_for_ack(FrameType.CONFIG, deadline)
        self._parse_config_ack(cfg_ack)

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

    def _wait_for_type(self, frame_type: type, deadline: float | None) -> AnyFrame:
        while True:
            f = self._recv_frame(deadline)
            if isinstance(f, frame_type):
                return f

    def _wait_for_ack(self, cmd_type: int, deadline: float | None) -> AckFrame:
        while True:
            f = self._recv_frame(deadline)
            if isinstance(f, AckFrame) and f.cmd_type == int(cmd_type):
                return f

    def _build_config_payload(self) -> bytes:
        if self.device_info is None:
            return b""
        ch_map = {ci.name: ci for ci in self.device_info.channels}
        payload = b""
        for ch in self._channels:
            ci = ch_map.get(ch.channel)
            if ci is None or ci.channel_type != ChannelType.STREAM:
                continue
            cfg = struct.pack(">BB", 1, ch.sampling_time)  # enabled, sampling_time
            payload += bytes([ci.channel_id, len(cfg)]) + cfg
        return payload

    def _parse_config_ack(self, ack: AckFrame) -> None:
        data = ack.config_data
        i = 0
        while i + 2 <= len(data):
            ch_id   = data[i]
            cfg_len = data[i + 1]
            i += 2
            if cfg_len >= 4 and i + cfg_len <= len(data):
                self.actual_rates[ch_id] = struct.unpack_from(">I", data, i)[0]
            i += cfg_len

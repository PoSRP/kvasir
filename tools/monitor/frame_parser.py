from __future__ import annotations
from dataclasses import dataclass

from .protocol import MAGIC, FrameType


@dataclass
class StreamDataFrame:
    channel_id: int
    seq:        int
    samples:    list[int]


@dataclass
class AckFrame:
    cmd_type: int
    status:   int


AnyFrame = StreamDataFrame | AckFrame


class FrameParser:
    def __init__(self) -> None:
        self._buf:      bytearray      = bytearray()
        self._last_seq: dict[int, int] = {}
        self.seq_drops: int            = 0

    def feed(self, data: bytes | bytearray) -> list[AnyFrame]:
        self._buf += data
        frames: list[AnyFrame] = []
        while (f := self._try_parse()) is not None:
            frames.append(f)
        return frames

    def _try_parse(self) -> AnyFrame | None:
        while True:
            # Resync: drop bytes until 0xAD or buffer empty
            while self._buf and self._buf[0] != MAGIC:
                del self._buf[0]

            if len(self._buf) < 4:
                return None

            type_byte   = self._buf[1]
            payload_len = (self._buf[2] << 8) | self._buf[3]
            frame_size  = 4 + payload_len

            if len(self._buf) < frame_size:
                return None

            payload = bytes(self._buf[4:frame_size])
            del self._buf[:frame_size]

            result = self._decode(type_byte, payload)
            if result is not None:
                return result

    def _decode(self, type_byte: int, payload: bytes) -> AnyFrame | None:
        match type_byte:
            case FrameType.STREAM_DATA:
                return self._decode_stream_data(payload)
            case FrameType.ACK:
                return self._decode_ack(payload)
            case _:
                return None

    def _decode_stream_data(self, payload: bytes) -> StreamDataFrame | None:
        if len(payload) < 5:
            return None
        ch_id = payload[0]
        seq   = (payload[1] << 8) | payload[2]
        count = (payload[3] << 8) | payload[4]
        if len(payload) != 5 + count * 2:
            return None
        samples = [
            (payload[5 + i * 2] << 8) | payload[5 + i * 2 + 1]
            for i in range(count)
        ]
        last = self._last_seq.get(ch_id)
        if last is not None:
            expected = (last + 1) & 0xFFFF
            if seq != expected:
                self.seq_drops += (seq - expected) & 0xFFFF
        self._last_seq[ch_id] = seq
        return StreamDataFrame(channel_id=ch_id, seq=seq, samples=samples)

    def _decode_ack(self, payload: bytes) -> AckFrame | None:
        if len(payload) < 2:
            return None
        return AckFrame(cmd_type=payload[0], status=payload[1])

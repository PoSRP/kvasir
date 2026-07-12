import struct
from tools.monitor.session import Session, Channel
from tools.monitor.frame_parser import StreamDataFrame
from tools.monitor.protocol import MAGIC, FrameType, AckStatus


class FakePort:
    def __init__(self):
        self.rx = bytearray()
        self.tx = bytearray()

    def inject(self, data: bytes) -> None:
        self.rx += data

    def read(self, n: int = 1) -> bytes:
        chunk = bytes(self.rx[:n])
        del self.rx[:n]
        return chunk

    @property
    def in_waiting(self) -> int:
        return len(self.rx)

    def write(self, data: bytes) -> None:
        self.tx += data


def _frame(type_byte: int, payload: bytes) -> bytes:
    return bytes([MAGIC, type_byte, len(payload) >> 8, len(payload) & 0xFF]) + payload


def _ack(cmd_type: int, status: int = AckStatus.OK) -> bytes:
    return _frame(FrameType.ACK, bytes([cmd_type, status]))


def _channels() -> list[Channel]:
    return [Channel(channel_id=0, name='v', sampling_time=0)]


def _connect(port: FakePort, channels: list[Channel] | None = None) -> Session:
    session = Session(port, channels or _channels())
    port.inject(_ack(FrameType.STOP))
    port.inject(_ack(FrameType.CONFIG))
    port.inject(_ack(FrameType.START))
    session.connect()
    return session


def test_connect_sends_stop_first():
    port = FakePort()
    _connect(port)
    assert port.tx[0] == MAGIC
    assert port.tx[1] == int(FrameType.STOP)
    assert port.tx[2] == 0
    assert port.tx[3] == 0


def test_connect_sends_config_after_stop():
    port = FakePort()
    _connect(port)
    assert port.tx[4] == MAGIC
    assert port.tx[5] == int(FrameType.CONFIG)


def test_connect_config_payload_includes_channel():
    port = FakePort()
    _connect(port)
    assert port.tx[4 + 4] == 0
    assert port.tx[4 + 5] == 2
    assert port.tx[4 + 6] == 1
    assert port.tx[4 + 7] == 0


def test_connect_sends_start_frame():
    port = FakePort()
    _connect(port)
    tx = bytes(port.tx)
    found = any(tx[i] == MAGIC and tx[i + 1] == int(FrameType.START)
                for i in range(len(tx) - 1))
    assert found


def test_feed_returns_stream_frames():
    port    = FakePort()
    session = _connect(port)

    count = 3
    pl = bytes([0, 0, 0, 0, count]) + struct.pack(">3H", 10, 20, 30)
    data = _frame(FrameType.STREAM_DATA, pl)
    result = session.feed(data)
    assert len(result) == 1
    assert isinstance(result[0], StreamDataFrame)
    assert result[0].samples == [10, 20, 30]


def test_feed_filters_ack_frames():
    port    = FakePort()
    session = _connect(port)
    result  = session.feed(_ack(FrameType.STOP))
    assert result == []


def test_stop_sends_stop_frame():
    port    = FakePort()
    session = _connect(port)
    port.tx.clear()
    session.stop()
    assert port.tx[0] == MAGIC
    assert port.tx[1] == int(FrameType.STOP)
    assert port.tx[2] == 0
    assert port.tx[3] == 0

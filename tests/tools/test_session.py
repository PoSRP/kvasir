import struct
from tools.session import Session
from tools.config import Channel
from tools.frame_parser import StreamDataFrame
from tools.protocol import MAGIC, FrameType, AckStatus


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


def _device_info(ch_id: int = 0, ch_type: int = 0, name: str = "ADC_IN0") -> bytes:
    enc = name.encode()
    payload = bytes([0, 1, 1, ch_id, ch_type, len(enc)]) + enc  # fw_major=0, fw_minor=1, ch_count=1
    return _frame(FrameType.DEVICE_INFO, payload)


def _ack(cmd_type: int, status: int = AckStatus.OK, config_data: bytes = b"") -> bytes:
    return _frame(FrameType.ACK, bytes([cmd_type, status]) + config_data)


def _config_ack(ch_id: int = 0, actual_rate: int = 535_000) -> bytes:
    extra = bytes([ch_id, 4]) + struct.pack(">I", actual_rate)
    return _ack(FrameType.CONFIG, AckStatus.OK, extra)


def _start_ack() -> bytes:
    return _ack(FrameType.START)


def _channels() -> list[Channel]:
    return [Channel(channel='ADC_IN0', name='v', sampling_time=0)]


def _connect(port: FakePort, channels: list[Channel] | None = None) -> Session:
    session = Session(port, channels or _channels())
    port.inject(_device_info())
    port.inject(_config_ack())
    port.inject(_start_ack())
    session.connect()
    return session


# ── connect() ────────────────────────────────────────────────────────────────

def test_connect_stores_device_info():
    port    = FakePort()
    session = _connect(port)
    assert session.device_info is not None
    assert len(session.device_info.channels) == 1
    assert session.device_info.channels[0].name == "ADC_IN0"


def test_connect_sends_config_frame():
    port = FakePort()
    _connect(port)
    # connect() writes STOP first (4 bytes: MAGIC, STOP, 0, 0), then CONFIG.
    assert port.tx[0] == MAGIC
    assert port.tx[1] == int(FrameType.STOP)
    assert port.tx[4] == MAGIC
    assert port.tx[5] == int(FrameType.CONFIG)


def test_connect_config_payload_includes_channel():
    port = FakePort()
    _connect(port)
    # After the 4-byte STOP frame the CONFIG frame starts at offset 4:
    # [MAGIC, 0x80, len_hi, len_lo, ch_id=0, cfg_len=2, enabled=1, samp_time=0]
    assert port.tx[4 + 4] == 0   # ch_id
    assert port.tx[4 + 5] == 2   # cfg_len
    assert port.tx[4 + 6] == 1   # enabled
    assert port.tx[4 + 7] == 0   # sampling_time


def test_connect_sends_start_frame():
    port = FakePort()
    _connect(port)
    # Find the START frame (0xAD 0x81) in tx after the CONFIG frame
    tx = bytes(port.tx)
    found = any(tx[i] == MAGIC and tx[i + 1] == int(FrameType.START)
                for i in range(len(tx) - 1))
    assert found


def test_connect_stores_actual_rate():
    port    = FakePort()
    session = _connect(port)
    assert session.actual_rates[0] == 535_000


def test_connect_actual_rate_from_ack():
    port = FakePort()
    port.inject(_device_info())
    port.inject(_config_ack(ch_id=0, actual_rate=428_571))
    port.inject(_start_ack())
    session = Session(port, _channels())
    session.connect()
    assert session.actual_rates[0] == 428_571


# ── feed() ────────────────────────────────────────────────────────────────────

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


def test_feed_filters_device_info_frames():
    port    = FakePort()
    session = _connect(port)
    result  = session.feed(_device_info())
    assert result == []


# ── stop() ───────────────────────────────────────────────────────────────────

def test_stop_sends_stop_frame():
    port    = FakePort()
    session = _connect(port)
    port.tx.clear()
    session.stop()
    assert port.tx[0] == MAGIC
    assert port.tx[1] == int(FrameType.STOP)
    # Empty payload
    assert port.tx[2] == 0
    assert port.tx[3] == 0


# ── unknown channel in config ─────────────────────────────────────────────────

def test_connect_ignores_config_channel_not_in_device_info():
    port = FakePort()
    # Config has ADC_IN9 but device only advertises ADC_IN0
    channels = [Channel(channel='ADC_IN9', name='v')]
    port.inject(_device_info(name="ADC_IN0"))
    port.inject(_config_ack())
    port.inject(_start_ack())
    session = Session(port, channels)
    session.connect()
    # CONFIG payload should be empty (no matching channels)
    config_frame_len = (port.tx[2] << 8) | port.tx[3]
    assert config_frame_len == 0

import struct
from tools.frame_parser import (
    FrameParser, DeviceInfoFrame, StreamDataFrame, AckFrame, ChannelInfo,
)
from tools.protocol import MAGIC, FrameType


def _frame(type_byte: int, payload: bytes) -> bytes:
    return bytes([MAGIC, type_byte, len(payload) >> 8, len(payload) & 0xFF]) + payload


def make_stream(seq: int, samples: list[int], ch_id: int = 0) -> bytes:
    count = len(samples)
    pl = bytes([ch_id, seq >> 8, seq & 0xFF, count >> 8, count & 0xFF])
    pl += struct.pack(f">{count}H", *samples)
    return _frame(FrameType.STREAM_DATA, pl)


def make_device_info(fw_major: int, fw_minor: int,
                     channels: list[tuple[int, int, str]]) -> bytes:
    body = bytes([fw_major, fw_minor, len(channels)])
    for ch_id, ch_type, name in channels:
        enc = name.encode()
        body += bytes([ch_id, ch_type, len(enc)]) + enc
    return _frame(FrameType.DEVICE_INFO, body)


def make_ack(cmd_type: int, status: int, config_data: bytes = b"") -> bytes:
    return _frame(FrameType.ACK, bytes([cmd_type, status]) + config_data)


# ── StreamDataFrame ───────────────────────────────────────────────────────────

def test_stream_single_frame():
    p = FrameParser()
    frames = p.feed(make_stream(0, [100, 200, 300]))
    assert len(frames) == 1
    f = frames[0]
    assert isinstance(f, StreamDataFrame)
    assert f.channel_id == 0
    assert f.seq == 0
    assert f.samples == [100, 200, 300]


def test_stream_channel_id():
    p = FrameParser()
    frames = p.feed(make_stream(0, [1], ch_id=7))
    assert frames[0].channel_id == 7


def test_stream_multiple_back_to_back():
    p = FrameParser()
    data = make_stream(0, [1, 2]) + make_stream(1, [3, 4])
    frames = p.feed(data)
    assert len(frames) == 2
    assert frames[0].seq == 0
    assert frames[1].seq == 1


def test_stream_partial_buffered():
    p    = FrameParser()
    data = make_stream(0, [10, 20])
    assert p.feed(data[:4]) == []
    frames = p.feed(data[4:])
    assert len(frames) == 1
    assert frames[0].samples == [10, 20]


def test_stream_no_drop_first_frame():
    p = FrameParser()
    p.feed(make_stream(500, [1]))
    assert p.seq_drops == 0


def test_stream_seq_drop_counted():
    p = FrameParser()
    p.feed(make_stream(0, [1]))
    p.feed(make_stream(3, [2]))
    assert p.seq_drops == 2


def test_stream_seq_wrap():
    p = FrameParser()
    p.feed(make_stream(0xFFFE, [1]))
    p.feed(make_stream(0xFFFF, [2]))
    p.feed(make_stream(0x0000, [3]))
    assert p.seq_drops == 0


def test_stream_samples_preserved():
    samples = list(range(256))
    p       = FrameParser()
    frames  = p.feed(make_stream(0, samples))
    assert frames[0].samples == samples


# ── DeviceInfoFrame ───────────────────────────────────────────────────────────

def test_device_info_parsed():
    p = FrameParser()
    data = make_device_info(0, 1, [(0, 0, "ADC_IN0"), (9, 1, "SPI1")])
    frames = p.feed(data)
    assert len(frames) == 1
    f = frames[0]
    assert isinstance(f, DeviceInfoFrame)
    assert f.fw_major == 0
    assert f.fw_minor == 1
    assert len(f.channels) == 2
    assert f.channels[0] == ChannelInfo(channel_id=0, channel_type=0, name="ADC_IN0")
    assert f.channels[1] == ChannelInfo(channel_id=9, channel_type=1, name="SPI1")


def test_device_info_partial_buffered():
    p    = FrameParser()
    data = make_device_info(0, 1, [(0, 0, "ADC_IN0")])
    assert p.feed(data[:3]) == []
    frames = p.feed(data[3:])
    assert len(frames) == 1
    assert isinstance(frames[0], DeviceInfoFrame)


# ── AckFrame ──────────────────────────────────────────────────────────────────

def test_ack_parsed():
    p      = FrameParser()
    frames = p.feed(make_ack(0x81, 0x00))
    assert len(frames) == 1
    f = frames[0]
    assert isinstance(f, AckFrame)
    assert f.cmd_type == 0x81
    assert f.status == 0x00
    assert f.config_data == b""


def test_ack_config_data():
    extra  = bytes([0x00, 0x04, 0x00, 0x08, 0x27, 0x10])
    p      = FrameParser()
    frames = p.feed(make_ack(0x80, 0x00, extra))
    assert frames[0].config_data == extra


# ── Unknown type skip / resync ────────────────────────────────────────────────

def test_unknown_type_skipped_via_length():
    p     = FrameParser()
    # Unknown type 0x05 with 3-byte payload, followed by a STREAM_DATA
    unk   = _frame(0x05, b"\x01\x02\x03")
    known = make_stream(7, [99])
    frames = p.feed(unk + known)
    assert len(frames) == 1
    assert isinstance(frames[0], StreamDataFrame)
    assert frames[0].seq == 7


def test_multiple_unknown_types_before_known():
    p   = FrameParser()
    buf = _frame(0x20, b"hello") + _frame(0x05, b"") + make_stream(0, [42])
    frames = p.feed(buf)
    assert len(frames) == 1
    assert frames[0].samples == [42]


def test_resyncs_after_garbage():
    p      = FrameParser()
    data   = b"\x00\xff\x12" + make_stream(0, [42])
    frames = p.feed(data)
    assert len(frames) == 1
    assert frames[0].samples == [42]


def test_mixed_types_in_sequence():
    p   = FrameParser()
    buf = (
        make_device_info(0, 1, [(0, 0, "ADC_IN0")])
        + make_ack(0x81, 0x00)
        + make_stream(0, [1, 2, 3])
    )
    frames = p.feed(buf)
    assert len(frames) == 3
    assert isinstance(frames[0], DeviceInfoFrame)
    assert isinstance(frames[1], AckFrame)
    assert isinstance(frames[2], StreamDataFrame)

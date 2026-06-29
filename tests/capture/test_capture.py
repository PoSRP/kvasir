import re
import struct


def make_stream_frame(seq: int, samples: list[int], ch_id: int = 0) -> bytes:
    count = len(samples)
    payload_len = 5 + count * 2
    return struct.pack(f">BBHBHH{count}H", 0xAD, 0x01, payload_len, ch_id, seq, count, *samples)


def drops_from_stderr(stderr: str) -> int:
    m = re.search(r"drops:\s*(\d+)", stderr)
    return int(m.group(1)) if m else 0


def test_single_frame_samples_written(capture):
    samples = list(range(256))
    got, _ = capture(make_stream_frame(0, samples))
    assert got == samples


def test_multiple_frames_concatenated(capture):
    f0 = make_stream_frame(0, [10, 20, 30])
    f1 = make_stream_frame(1, [40, 50, 60])
    got, _ = capture(f0 + f1)
    assert got == [10, 20, 30, 40, 50, 60]


def test_partial_frame_at_end_ignored(capture):
    complete   = make_stream_frame(0, [1, 2, 3])
    incomplete = complete[:4]          # truncated mid-frame
    got, _ = capture(complete + incomplete)
    assert got == [1, 2, 3]


def test_garbage_prefix_resyncs(capture):
    garbage = bytes([0x00, 0xFF, 0x01, 0x02])
    got, _ = capture(garbage + make_stream_frame(0, [7, 8, 9]))
    assert got == [7, 8, 9]


def test_no_drops_sequential(capture):
    data = b"".join(make_stream_frame(i, [i * 10]) for i in range(8))
    _, stderr = capture(data)
    assert drops_from_stderr(stderr) == 0


def test_seq_drop_reported(capture):
    f0 = make_stream_frame(0, [1])
    f3 = make_stream_frame(3, [2])   # seq 1 and 2 missing
    _, stderr = capture(f0 + f3)
    assert drops_from_stderr(stderr) == 2

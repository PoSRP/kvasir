import numpy as np
import pytest

from tools.monitor import (
    _DisplayBuffer, _Board, Monitor, DISPLAY_POINTS, short_tag, compute_tags,
)
from tools.config import Config, Channel
from tools.frame_parser import StreamDataFrame


def _monitor_with_board(scale: float = 1.0, offset: float = 0.0,
                        window: float = 1.0) -> tuple[Monitor, _Board]:
    """Monitor with one configured board, one channel mapped to ch_id=0, no real port."""
    ch  = Channel(channel='ADC_IN0', name='v', scale=scale, offset=offset)
    cfg = Config(boards={'ABC123': [ch]}, window=window)
    m   = Monitor(cfg)
    board = m._boards['ABC123']
    board.ch_id_map = {0: ch}
    board.buffers   = {'v': _DisplayBuffer(DISPLAY_POINTS, window)}
    return m, board


# ── helpers ───────────────────────────────────────────────────────────────────

def test_short_tag_last_four():
    assert short_tag('37A436773235') == '3235'
    assert short_tag('abcd') == 'ABCD'


def test_compute_tags_single_uses_suffix():
    tags = compute_tags(['37A436773235'])
    assert tags == {'37A436773235': '3235'}


def test_compute_tags_falls_back_to_prefix_when_suffix_collides():
    # The two boards in our test setup share their last 8 chars but differ
    # in the first 4 — exercise the prefix fallback.
    tags = compute_tags(['37A436773235', '376136773235'])
    assert tags == {'37A436773235': '37A4', '376136773235': '3761'}


def test_compute_tags_extends_suffix_when_possible():
    # All three share the last 4 (XXXX), but pairs differ at length 5.
    tags = compute_tags(['1XXXX', '2YXXXX', '3ZXXXX'])
    assert len(set(tags.values())) == 3


# ── _DisplayBuffer ────────────────────────────────────────────────────────────

def test_display_buffer_view_length():
    buf = _DisplayBuffer(2000, DISPLAY_POINTS)
    assert len(buf.view()) == DISPLAY_POINTS


def test_display_buffer_empty_view_all_nan():
    buf = _DisplayBuffer(2000, DISPLAY_POINTS)
    assert np.all(np.isnan(buf.view()))


def test_display_buffer_empty_data_range():
    buf = _DisplayBuffer(2000, DISPLAY_POINTS)
    assert buf.data_range() is None


def test_display_buffer_push_one_bin():
    buf = _DisplayBuffer(100, 10)
    buf.push(np.ones(10, dtype=np.float32))
    valid = buf.view()[~np.isnan(buf.view())]
    assert len(valid) == 1
    assert valid[0] == pytest.approx(1.0)


def test_display_buffer_mean_of_bin():
    buf  = _DisplayBuffer(100, 10)
    data = np.arange(0, 20, 2, dtype=np.float32)
    buf.push(data)
    valid = buf.view()[~np.isnan(buf.view())]
    assert len(valid) == 1
    assert valid[0] == pytest.approx(9.0)


# ── Monitor.on_stream_frame ───────────────────────────────────────────────────

def test_on_stream_frame_pushes_into_buffer():
    m, board = _monitor_with_board()
    m.on_stream_frame(board, StreamDataFrame(channel_id=0, seq=0, samples=[2048] * 64))
    assert np.any(~np.isnan(board.buffers['v'].view()))


def test_on_stream_frame_applies_scale_and_offset():
    m, board = _monitor_with_board(scale=2.0, offset=1.0)
    m.on_stream_frame(board, StreamDataFrame(channel_id=0, seq=0, samples=[100] * 64))
    valid = board.buffers['v'].view()
    valid = valid[~np.isnan(valid)]
    assert len(valid) > 0
    assert valid[0] == pytest.approx(201.0)  # 100 * 2 + 1


def test_on_stream_frame_unknown_channel_ignored():
    m, board = _monitor_with_board()
    m.on_stream_frame(board, StreamDataFrame(channel_id=7, seq=0, samples=[500] * 64))
    assert np.all(np.isnan(board.buffers['v'].view()))


def test_on_stream_frame_increments_sample_counter():
    m, board = _monitor_with_board()
    assert m._samples_in == 0
    m.on_stream_frame(board, StreamDataFrame(channel_id=0, seq=0, samples=[0] * 64))
    assert m._samples_in == 64


def test_on_stream_frame_routes_per_board():
    """Two boards with identical channel layouts but different scaling — frames on
    each board's ch_id_map should route to that board's buffer only."""
    cha = Channel(channel='ADC_IN0', name='v', scale=1.0, offset=0.0)
    chb = Channel(channel='ADC_IN0', name='v', scale=10.0, offset=0.0)
    cfg = Config(boards={'AAAA': [cha], 'BBBB': [chb]}, window=1.0)
    m   = Monitor(cfg)
    a, b = m._boards['AAAA'], m._boards['BBBB']
    a.ch_id_map = {0: cha}
    a.buffers   = {'v': _DisplayBuffer(DISPLAY_POINTS, 1.0)}
    b.ch_id_map = {0: chb}
    b.buffers   = {'v': _DisplayBuffer(DISPLAY_POINTS, 1.0)}

    m.on_stream_frame(a, StreamDataFrame(channel_id=0, seq=0, samples=[1] * 64))
    m.on_stream_frame(b, StreamDataFrame(channel_id=0, seq=0, samples=[1] * 64))

    va = a.buffers['v'].view()
    va = va[~np.isnan(va)]
    vb = b.buffers['v'].view()
    vb = vb[~np.isnan(vb)]
    assert va[0] == pytest.approx(1.0)   # scale 1
    assert vb[0] == pytest.approx(10.0)  # scale 10

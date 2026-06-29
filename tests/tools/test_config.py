import pathlib
import pytest
from tools.config import load, Channel, _num

EXAMPLE = pathlib.Path(__file__).parent / 'example.yaml'


def test_loads_boards():
    cfg = load(EXAMPLE)
    assert list(cfg.boards.keys()) == ['ABC123']


def test_loads_channels_for_board():
    cfg = load(EXAMPLE)
    chans = cfg.boards['ABC123']
    assert len(chans) == 2
    assert chans[0].name == 'voltage'
    assert chans[1].name == 'current'


def test_channel_units():
    cfg = load(EXAMPLE)
    chans = cfg.boards['ABC123']
    assert chans[0].unit == 'V'
    assert chans[1].unit == 'A'


def test_channel_defaults():
    ch = Channel(channel='ADC_IN0', name='test')
    assert ch.scale  == 1.0
    assert ch.offset == 0.0
    assert ch.unit   == 'raw'


def test_linear_conversion():
    ch = Channel(channel='ADC_IN0', name='v', scale=0.000806, offset=0.0)
    assert abs(ch.convert(4096) - 3.302) < 0.001


def test_offset_applied():
    ch = Channel(channel='ADC_IN0', name='i', scale=1.0, offset=-0.5)
    assert ch.convert(0) == pytest.approx(-0.5)
    assert ch.convert(1) == pytest.approx(0.5)


def test_window_default():
    cfg = load(EXAMPLE)
    assert cfg.window == 2.0


def test_log_default_null():
    cfg = load(EXAMPLE)
    assert cfg.log is None


def test_board_serials_uppercased(tmp_path):
    p = tmp_path / 'cfg.yaml'
    p.write_text(
        'boards:\n'
        '  abc123:\n'
        '    channels:\n'
        '      - {channel: ADC_IN0, name: v}\n'
    )
    cfg = load(p)
    assert list(cfg.boards.keys()) == ['ABC123']


def test_duplicate_channel_name_rejected(tmp_path):
    p = tmp_path / 'dup.yaml'
    p.write_text(
        'boards:\n'
        '  S1:\n'
        '    channels:\n'
        '      - {channel: ADC_IN0, name: x}\n'
        '      - {channel: ADC_IN1, name: x}\n'
    )
    with pytest.raises(ValueError, match='duplicate channel name'):
        load(p)


def test_invalid_boards_type_rejected(tmp_path):
    p = tmp_path / 'bad.yaml'
    p.write_text('boards: [a, b]\n')
    with pytest.raises(ValueError, match="'boards' must be a mapping"):
        load(p)


def test_num_int():
    assert _num(42) == 42.0


def test_num_float():
    assert _num(3.14) == pytest.approx(3.14)


def test_num_expression():
    assert _num("3.3 / (2^12 - 1)") == pytest.approx(3.3 / 4095)


def test_num_caret_is_power():
    assert _num("2^10") == pytest.approx(1024.0)


def test_num_none_returns_default():
    assert _num(None) is None


def test_num_none_custom_default():
    assert _num(None, 5.0) == 5.0

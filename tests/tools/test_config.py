import pathlib
import numpy as np
import pytest
from tools.monitor.monitor import load, _num, Channel

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
    ch = Channel(channel_id=0, name='test')
    assert ch.unit    == 'V'
    assert ch.formula is None


def test_default_conversion_to_volts():
    ch = Channel(channel_id=0, name='v')
    assert ch.convert(0)    == pytest.approx(0.0)
    assert ch.convert(4095) == pytest.approx(3.3)
    assert ch.convert(2047.5) == pytest.approx(1.65)


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


def test_formula_none_by_default():
    ch = Channel(channel_id=0, name='v')
    assert ch.formula is None


def test_formula_ntc_beta_at_25c():
    ch = Channel(
        channel_id=0, name='probe',
        formula='1/(1/298.15 + log(50000*v/(3.3-v)/50000)/3950) - 273.15',
    )
    assert float(ch.convert(2047.5)) == pytest.approx(25.0, abs=0.01)


def test_formula_sees_volts_not_counts():
    ch = Channel(channel_id=0, name='x', formula='v')
    assert ch.convert(4095) == pytest.approx(3.3)


def test_formula_vectorized_over_batch():
    ch  = Channel(channel_id=0, name='x', formula='log(v)')
    raw = np.array([1000, 2000, 3000], dtype=np.float64)
    np.testing.assert_allclose(ch.convert(raw), np.log(raw * 3.3 / 4095))


def test_formula_syntax_error_at_construction():
    with pytest.raises(ValueError, match='invalid formula'):
        Channel(channel_id=0, name='x', formula='1 + ')


def test_formula_unknown_name_rejected_at_construction():
    with pytest.raises(ValueError, match='unknown names'):
        Channel(channel_id=0, name='x', formula='os')


def test_formula_loads_from_config(tmp_path):
    p = tmp_path / 'cfg.yaml'
    p.write_text(
        'boards:\n'
        '  S1:\n'
        '    channels:\n'
        '      - {channel: ADC_IN0, name: t, formula: "v * 2 + 1"}\n'
    )
    cfg = load(p)
    ch = cfg.boards['S1'][0]
    assert ch.formula == 'v * 2 + 1'
    assert ch.convert(0) == pytest.approx(1.0)


def test_formula_null_in_yaml_is_none(tmp_path):
    p = tmp_path / 'cfg.yaml'
    p.write_text(
        'boards:\n'
        '  S1:\n'
        '    channels:\n'
        '      - {channel: ADC_IN0, name: t, formula: null}\n'
    )
    cfg = load(p)
    assert cfg.boards['S1'][0].formula is None

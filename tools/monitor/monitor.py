#!/usr/bin/env python3
"""Real-time monitor and data logger for kvasir sensor capture boards.

Usage:
    python3 tools/monitor/monitor.py
    python3 tools/monitor/monitor.py example.yaml
    python3 tools/monitor/monitor.py example.yaml --output run.h5
    python3 tools/monitor/monitor.py example.yaml --no-gui --output run.h5
"""
from __future__ import annotations
import argparse
import ast
import math
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Optional
import numpy as np
import serial
import yaml
from serial.tools import list_ports


USB_VID = 0x0483
USB_PID = 0x4B56

MAGIC = 0xAD

ADC_CLK_HZ   = 24_000_000
_SMPR_CYCLES = (3, 15, 28, 56, 84, 112, 144, 480)
ADC_VREF     = 3.3
ADC_MAX_CODE = 4095

_FORMULA_NAMES: dict[str, object] = {
    n: getattr(np, n) for n in (
        'log', 'log10', 'log2', 'exp', 'sqrt', 'sin', 'cos', 'tan',
        'arcsin', 'arccos', 'arctan', 'arctan2', 'abs', 'sign',
        'floor', 'ceil', 'clip', 'power', 'minimum', 'maximum',
        'pi', 'e',
    )
}

NUM_CH = 10  # kvasir board has ADC_IN0..ADC_IN9

DISPLAY_POINTS = 2000
LOG_CHUNK      = 65536

DEFAULT_CONFIG = Path(__file__).parent / 'example.yaml'


class FrameType(IntEnum):
    # Device -> host
    STREAM_DATA = 0x01
    ACK         = 0x10
    # Host -> device
    CONFIG      = 0x80
    START       = 0x81
    STOP        = 0x82


class AckStatus(IntEnum):
    OK    = 0x00
    ERROR = 0x01


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
        with np.errstate(divide='ignore', invalid='ignore'):
            return eval(self._code, {'v': v, **_FORMULA_NAMES})


@dataclass
class Config:
    # Insertion order is preserved; the monitor lays out plot rows in this order.
    boards: dict[str, list[Channel]] = field(default_factory=dict)
    log:    Optional[str]            = None
    window: float                    = 5.0  # seconds of history shown in the plot


@dataclass
class _Board:
    """Per-board runtime state. One per entry in config.boards."""
    serial:     str  # full USB serial (uppercased)
    tag:        str  # short tag for UI / log dataset prefix
    channels:   list[Channel]
    port_path:  str | None                    = None  # latest discovered device path
    port:       serial.Serial | None          = None
    session:    Session | None                = None
    connecting: bool                          = False
    ch_id_map:  dict[int, Channel]            = field(default_factory=dict)
    buffers:    dict[str, _DisplayBuffer]     = field(default_factory=dict)
    last_drops: int                           = 0  # last-observed session drop count, for delta detection


def _make_frame(type_byte: int, payload: bytes) -> bytes:
    return bytes([MAGIC, type_byte, len(payload) >> 8, len(payload) & 0xFF]) + payload


def compute_sample_rate(sampling_times: list[int]) -> int:
    """Per-channel sample rate when the ADC scans the given channels in series.

    All channels share the same rate: total scan cycles = sum over channels of
    (sampling + 12 conversion) cycles; per-channel rate = ADC_CLK / total.
    Returns 0 when no channels are enabled."""
    if not sampling_times:
        return 0
    total = sum(_SMPR_CYCLES[s] + 12 for s in sampling_times)
    return ADC_CLK_HZ // total


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


def _num(v, default=None):
    if v is None:
        return default
    if isinstance(v, (int, float)):
        return float(v)
    return float(eval(str(v).replace('^', '**'), {'__builtins__': {}}, vars(math)))


def _channel_name_to_id(name: str) -> int:
    prefix = 'ADC_IN'
    if name.startswith(prefix):
        rest = name[len(prefix):]
        if rest.isdigit():
            n = int(rest)
            if 0 <= n < NUM_CH:
                return n
    raise ValueError(f"channel '{name}' must be one of ADC_IN0..ADC_IN{NUM_CH - 1}")


def short_tag(serial_str: str) -> str:
    """Single-serial fallback: last 4 chars uppercased. For multi-board
    disambiguation use compute_tags() -- STM32 unique IDs often share their
    LSBs across siblings from the same lot, so 'last 4 chars' is not unique
    in the general case."""
    return serial_str[-4:].upper()


def compute_tags(serials: list[str]) -> dict[str, str]:
    """Map each serial to the shortest slice that disambiguates the set.
    Tries 4..N suffix lengths, then 4..N prefix lengths, finally falls back
    to the full serial. Suffix is preferred (matches the original intent)."""
    serials = [s.upper() for s in serials]
    if not serials:
        return {}
    if len(serials) == 1:
        return {serials[0]: short_tag(serials[0])}
    L = max(len(s) for s in serials)
    for n in range(4, L + 1):
        suf = {s: s[-n:] for s in serials}
        if len(set(suf.values())) == len(serials):
            return suf
        pre = {s: s[:n] for s in serials}
        if len(set(pre.values())) == len(serials):
            return pre
    return {s: s for s in serials}


def discover_kvasir_ports() -> dict[str, str]:
    """Map of (normalized USB serial) -> device path for every attached kvasir."""
    out: dict[str, str] = {}
    for p in list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID and p.serial_number:
            out[p.serial_number.upper()] = p.device
    return out


def _dataset_name(tag: str, name: str) -> str:
    return f'{tag}_{name}'


def _parse_channels(ch_list: list, where: str) -> list[Channel]:
    channels = [
        Channel(
            channel_id=_channel_name_to_id(str(ch['channel'])),
            name=str(ch.get('name', ch['channel'])),
            unit=str(ch.get('unit', 'V')),
            y_min=_num(ch.get('y_min')),
            y_max=_num(ch.get('y_max')),
            sampling_time=int(ch.get('sampling_time', 0)),
            formula=(str(ch['formula']) if ch.get('formula') else None),
        )
        for ch in ch_list
    ]
    for ch in channels:
        if not 0 <= ch.sampling_time <= 7:
            raise ValueError(
                f"{where}: channel '{ch.name}' sampling_time {ch.sampling_time} "
                f"out of range (must be 0-7; maps to 3/15/28/56/84/112/144/480 ADC cycles)"
            )
    seen: set[str] = set()
    for ch in channels:
        if ch.name in seen:
            raise ValueError(f"{where}: duplicate channel name '{ch.name}'")
        seen.add(ch.name)
    return channels


def load(path: str) -> Config:
    with open(path) as f:
        data = yaml.safe_load(f) or {}
    raw_boards = data.get('boards') or {}
    if not isinstance(raw_boards, dict):
        raise ValueError("'boards' must be a mapping of USB serial -> {channels: [...]}")
    boards: dict[str, list[Channel]] = {}
    for serial_key, entry in raw_boards.items():
        key = str(serial_key).upper()
        if key in boards:
            raise ValueError(f"duplicate board serial '{key}'")
        ch_list = (entry or {}).get('channels', [])
        boards[key] = _parse_channels(ch_list, f"board {key}")
    return Config(
        boards=boards,
        log=data.get('log'),
        window=float(data.get('window', 5.0)),
    )


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


class _DisplayBuffer:
    """Rolling time-window display buffer driven by wall-clock arrival time.

    Divides the window into n_points equal time slots and averages whatever
    samples arrive in each slot. Slots with no data show as NaN. view()
    always returns n_points values in chronological order (oldest first).
    """

    def __init__(self, n_points: int, window: float) -> None:
        self._n   = n_points
        self._dt  = window / n_points
        self._buf = np.full(n_points, np.nan, dtype=np.float32)
        self._t0  = time.monotonic()
        self._cur = -1
        self._acc = 0.0
        self._cnt = 0

    def push(self, arr: np.ndarray) -> None:
        t    = time.monotonic() - self._t0
        slot = int(t / self._dt)
        if slot != self._cur:
            if self._cur >= 0 and self._cnt > 0:
                self._buf[self._cur % self._n] = self._acc / self._cnt
            start = max(self._cur + 1, slot - self._n + 1)
            for s in range(start, slot):
                self._buf[s % self._n] = np.nan
            self._cur = slot
            self._acc = float(arr.mean())
            self._cnt = 1
        else:
            self._acc += float(arr.mean())
            self._cnt += 1

    def view(self) -> np.ndarray:
        if self._cur >= 0 and self._cnt > 0:
            self._buf[self._cur % self._n] = self._acc / self._cnt
        start = (self._cur + 1) % self._n if self._cur >= 0 else 0
        arr = np.concatenate([self._buf[start:], self._buf[:start]])
        nans = np.isnan(arr)
        if nans.any() and not nans.all():
            valid_idx = np.where(~nans)[0]
            lo, hi    = valid_idx[0], valid_idx[-1]
            interior  = np.where(nans & (np.arange(len(arr)) > lo)
                                       & (np.arange(len(arr)) < hi))[0]
            if len(interior):
                arr[interior] = np.interp(interior, valid_idx, arr[~nans])
        return arr

    def data_range(self) -> tuple[float, float] | None:
        valid = self._buf[~np.isnan(self._buf)]
        if not len(valid):
            return None
        return float(valid.min()), float(valid.max())


class Monitor:
    """Multi-board monitor. Discovers kvasir devices by USB VID/PID + serial,
    spawns one Session per configured board, and renders all channels in one window."""

    _warned_unknown: set[str] = set()  # class-level so it survives across run calls

    def __init__(self, config: Config, log_format: str = 'hdf5') -> None:
        self._cfg    = config
        self._log_format = log_format
        tags         = compute_tags(list(config.boards.keys()))
        self._boards: dict[str, _Board] = {
            serial_key: _Board(serial=serial_key, tag=tags[serial_key], channels=chans)
            for serial_key, chans in config.boards.items()
        }
        self._log:       object | None              = None
        self._datasets:  dict[str, object]          = {}
        self._pending:   dict[str, list[np.ndarray]] = {}
        self._raw_files: dict[str, object]          = {}
        self._bytes_in    = 0
        self._samples_in  = 0
        self._frames_received = 0
        self._frames_dropped  = 0
        self._t_stats     = time.monotonic()
        self._last_stats  = ''

    def on_stream_frame(self, board: _Board, frame: StreamDataFrame) -> None:
        """Test seam: dispatch one decoded STREAM_DATA frame onto board's buffers and log."""
        self._frames_received += 1
        ch = board.ch_id_map.get(frame.channel_id)
        if ch is None:
            return
        raw = np.array(frame.samples, dtype=np.float32)
        eng = ch.convert(raw)
        buf = board.buffers.get(ch.name)
        if buf is not None:
            buf.push(eng)
        self._samples_in += len(eng)
        if not self._log:
            return
        key = _dataset_name(board.tag, ch.name)
        if self._log_format == 'raw':
            self._raw_files[key].write(np.array(frame.samples, dtype='<u2').tobytes())
        else:
            self._pending[key].append(eng)
            if sum(len(p) for p in self._pending[key]) >= LOG_CHUNK:
                self._flush_log()

    def run(self, gui: bool = True) -> None:
        if gui:
            self._run_gui()
        else:
            self._run_headless()

    def _open_log(self, path: str) -> None:
        if self._log_format == 'raw':
            self._open_raw_log(path)
        else:
            self._open_hdf5_log(path)

    def _open_raw_log(self, prefix: str) -> None:
        # Sentinel: on_stream_frame only writes when _log is truthy.
        self._log = True
        for board in self._boards.values():
            for ch in board.channels:
                key = _dataset_name(board.tag, ch.name)
                self._raw_files[key] = open(f'{prefix}_{key}.bin', 'wb')

    def _open_hdf5_log(self, path: str) -> None:
        import h5py
        self._log = h5py.File(path, 'w')
        for board in self._boards.values():
            for ch in board.channels:
                key = _dataset_name(board.tag, ch.name)
                ds  = self._log.create_dataset(
                    key, shape=(0,), maxshape=(None,),
                    dtype='float32', chunks=(LOG_CHUNK,),
                )
                ds.attrs['unit']   = ch.unit
                ds.attrs['serial'] = board.serial
                if ch.formula:
                    ds.attrs['formula'] = ch.formula
                self._datasets[key] = ds
                self._pending[key]  = []

    def _flush_log(self) -> None:
        for key, chunks in self._pending.items():
            if not chunks:
                continue
            arr = np.concatenate(chunks)
            ds  = self._datasets[key]
            ds.resize(ds.shape[0] + len(arr), axis=0)
            ds[-len(arr):] = arr
            self._pending[key] = []

    def _refresh_port_paths(self) -> None:
        """Rescan USB and update port_path on every configured board.
        Boards present in config but not currently attached have port_path=None."""
        present = discover_kvasir_ports()
        for s, board in self._boards.items():
            board.port_path = present.get(s)
        unknown = set(present) - set(self._boards)
        for s in unknown - self._warned_unknown:
            print(f'Ignoring unconfigured kvasir on {present[s]} (serial {s}).',
                  file=sys.stderr)
            self._warned_unknown.add(s)

    def _try_connect(self, board: _Board) -> bool:
        if board.port_path is None:
            return False
        try:
            port = serial.Serial(board.port_path, timeout=0.05)
            port.dtr = False
            port.reset_input_buffer()
            port.dtr = True
        except serial.SerialException:
            return False
        try:
            session = Session(port, board.channels)
            session.connect()
        except Exception:
            port.close()
            return False

        board.ch_id_map = {ch.channel_id: ch for ch in board.channels}
        for ch in board.channels:
            if ch.name not in board.buffers:
                board.buffers[ch.name] = _DisplayBuffer(DISPLAY_POINTS, self._cfg.window)
        rate = compute_sample_rate([ch.sampling_time for ch in board.channels])
        if self._log:
            for ch in board.channels:
                key = _dataset_name(board.tag, ch.name)
                if key in self._datasets:
                    self._datasets[key].attrs['sample_rate'] = rate

        print(f'[{board.tag}] connected on {board.port_path}, '
              f'{len(board.channels)} channel(s) @ {rate} Sa/s per channel',
              file=sys.stderr)

        board.port       = port
        board.session    = session
        board.last_drops = 0
        return True

    def _disconnect(self, board: _Board) -> None:
        if board.session:
            try:
                board.session.stop()
            except Exception:
                pass
            board.session.close()
        elif board.port:
            board.port.close()
        board.port      = None
        board.session   = None
        board.ch_id_map = {}

    def _read_board(self, board: _Board) -> bool:
        try:
            n = board.port.in_waiting
            data = board.port.read(n if n else 1)
            self._bytes_in += len(data)
            for frame in board.session.feed(data):
                self.on_stream_frame(board, frame)
            current = board.session._parser.seq_drops
            new_drops = current - board.last_drops
            if new_drops > 0:
                self._frames_dropped += new_drops
                board.last_drops     = current
                total = self._frames_received + self._frames_dropped
                print(f'Dropped USB packet ({self._frames_dropped} of {total})',
                      file=sys.stderr)
            return True
        except serial.SerialException:
            return False

    def _stats(self) -> str:
        connected = [b for b in self._boards.values() if b.session is not None]
        if not connected:
            return ''
        now = time.monotonic()
        dt  = now - self._t_stats
        if dt < 1.0:
            return self._last_stats
        mb_s  = self._bytes_in   / dt / 1e6
        ksa_s = self._samples_in / dt / 1e3
        drops = sum(b.session._parser.seq_drops for b in connected)
        self._bytes_in = self._samples_in = 0
        self._t_stats  = now
        self._last_stats = (f'{len(connected)}/{len(self._boards)} boards  '
                            f'{mb_s:.2f} MB/s  {ksa_s:.0f} kSa/s  drops:{drops}')
        return self._last_stats

    def _close(self) -> None:
        if self._log:
            if self._log_format == 'raw':
                for f in self._raw_files.values():
                    f.close()
            else:
                self._flush_log()
                self._log.close()
        for board in self._boards.values():
            self._disconnect(board)

    def _window_title(self) -> str:
        if not self._boards:
            return 'Kvasir Monitor -- no boards configured'
        parts = []
        for board in self._boards.values():
            mark = '*' if board.session is not None else '...'
            parts.append(f'[{board.tag}]{mark}')
        return 'Kvasir Monitor -- ' + ' '.join(parts)

    def _service_connections(self, background: bool) -> None:
        """Try to connect any board that isn't connected yet."""
        self._refresh_port_paths()
        for board in self._boards.values():
            if board.session is not None or board.connecting:
                continue
            if board.port_path is None:
                continue
            if background:
                board.connecting = True
                def _bg(b=board) -> None:
                    try:
                        self._try_connect(b)
                    finally:
                        b.connecting = False
                threading.Thread(target=_bg, daemon=True).start()
            else:
                self._try_connect(board)

    def _run_headless(self) -> None:
        if not self._boards:
            print('No boards in config; nothing to do.', file=sys.stderr)
            return
        if self._cfg.log:
            self._open_log(self._cfg.log)
        last_refresh = 0.0
        try:
            while True:
                now = time.monotonic()
                if now - last_refresh > 0.5:
                    self._service_connections(background=False)
                    last_refresh = now

                read_any = False
                for board in self._boards.values():
                    if board.session is None:
                        continue
                    if self._read_board(board):
                        read_any = True
                    else:
                        print(f'Lost [{board.tag}] on {board.port_path}, reconnecting...',
                              file=sys.stderr)
                        self._disconnect(board)

                if not read_any:
                    time.sleep(0.02)

                s = self._stats()
                if s and s != self._last_stats:
                    pass
                if s and now - getattr(self, '_t_print', 0.0) >= 1.0:
                    print(s, file=sys.stderr)
                    self._t_print = now
        except KeyboardInterrupt:
            pass
        finally:
            self._close()

    def _run_gui(self) -> None:
        import signal
        import pyqtgraph as pg
        from pyqtgraph.Qt import QtCore

        if not self._boards:
            print('No boards in config; nothing to do.', file=sys.stderr)
            return
        if self._cfg.log:
            self._open_log(self._cfg.log)

        total_rows = sum(len(b.channels) for b in self._boards.values())

        app = pg.mkQApp('Kvasir Monitor')
        # Qt's C event loop blocks Python's signal handling; a SIGINT handler that
        # quits the app plus a no-op timer to yield to Python lets Ctrl+C kill the UI.
        signal.signal(signal.SIGINT, lambda *_: app.quit())
        sigint_timer = QtCore.QTimer()
        sigint_timer.timeout.connect(lambda: None)
        sigint_timer.start(100)

        win = pg.GraphicsLayoutWidget(title=self._window_title())
        win.resize(1200, 220 * max(1, total_rows))

        # Build plots in config order: board1.ch0, board1.ch1, ..., board2.ch0, ...
        plot_curves: list[tuple[_Board, Channel, object, object]] = []
        row = 0
        hues = max(1, total_rows)
        for board in self._boards.values():
            for ch in board.channels:
                plot = win.addPlot(row=row, col=0,
                                   title=f'[{board.tag}] {ch.name} [{ch.unit}]')
                plot.setLabel('bottom', 'time', units='s')
                plot.setLabel('left', ch.unit)
                plot.getAxis('left').enableAutoSIPrefix(False)
                plot.showGrid(x=True, y=True, alpha=0.3)
                plot.enableAutoRange(axis='y', enable=False)
                if ch.y_min is not None and ch.y_max is not None:
                    plot.setYRange(ch.y_min, ch.y_max, padding=0)
                curve = plot.plot(pen=pg.intColor(row, hues=hues))
                plot_curves.append((board, ch, plot, curve))
                row += 1

        x = np.linspace(-self._cfg.window, 0, DISPLAY_POINTS)

        def read() -> None:
            self._service_connections(background=True)
            for board in self._boards.values():
                if board.session is None:
                    continue
                if not self._read_board(board):
                    self._disconnect(board)

        def update() -> None:
            for board, ch, plot, curve in plot_curves:
                buf = board.buffers.get(ch.name)
                if buf is None:
                    continue
                curve.setData(x[-DISPLAY_POINTS:], buf.view())
                if ch.y_min is None or ch.y_max is None:
                    r = buf.data_range()
                    if r:
                        lo, hi = r
                        span = (hi - lo) or 1.0
                        plot.setYRange(lo - span * 0.05, hi + span * 0.05, padding=0)
            s     = self._stats()
            title = self._window_title()
            win.setWindowTitle(f'{title}  |  {s}' if s else title)

        read_timer = QtCore.QTimer()
        read_timer.timeout.connect(read)
        read_timer.start(5)

        display_timer = QtCore.QTimer()
        display_timer.timeout.connect(update)
        display_timer.start(50)

        win.show()
        try:
            pg.exec()
        finally:
            sigint_timer.stop()
            read_timer.stop()
            display_timer.stop()
            self._close()


def main() -> None:
    ap = argparse.ArgumentParser(description='Kvasir sensor capture monitor')
    ap.add_argument('config', nargs='?', default=str(DEFAULT_CONFIG),
                    help=f'YAML config file (default: {DEFAULT_CONFIG.name})')
    ap.add_argument('--no-gui', action='store_true',
                    help='headless logging, no display required')
    ap.add_argument('--output', metavar='PATH',
                    help='log output path (overrides config log). '
                         'For --format hdf5, an .h5 file. For --format raw, a '
                         'filename prefix; one <prefix>_<tag>_<channel>.bin '
                         'is written per channel with uint16 little-endian samples.')
    ap.add_argument('--format', choices=('hdf5', 'raw'), default='hdf5',
                    help='log format (default: hdf5)')
    args = ap.parse_args()

    cfg = load(args.config)
    if args.output:
        cfg.log = args.output

    Monitor(cfg, log_format=args.format).run(gui=not args.no_gui)


if __name__ == '__main__':
    main()

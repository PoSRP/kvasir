#!/usr/bin/env python3
"""Real-time monitor and data logger for kvasir sensor capture boards.

Usage:
    python3 -m tools.monitor
    python3 -m tools.monitor example.yaml
    python3 -m tools.monitor example.yaml --output run.h5
    python3 -m tools.monitor example.yaml --no-gui --output run.h5
"""
from __future__ import annotations
import argparse
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
import numpy as np
import serial
from serial.tools import list_ports

from .frame_parser import StreamDataFrame
from .config import load as load_config, Config, Channel
from .session import Session
from .protocol import USB_VID, USB_PID

DISPLAY_POINTS = 2000
LOG_CHUNK      = 65536

DEFAULT_CONFIG = Path(__file__).parent / 'example.yaml'


def short_tag(serial: str) -> str:
    """Single-serial fallback: last 4 chars uppercased. For multi-board
    disambiguation use compute_tags() — STM32 unique IDs often share their
    LSBs across siblings from the same lot, so 'last 4 chars' is not unique
    in the general case."""
    return serial[-4:].upper()


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
    """Map of (normalized USB serial) → device path for every attached kvasir."""
    out: dict[str, str] = {}
    for p in list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID and p.serial_number:
            out[p.serial_number.upper()] = p.device
    return out


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


@dataclass
class _Board:
    """Per-board runtime state. One per entry in config.boards."""
    serial:     str                              # full USB serial (uppercased)
    tag:        str                              # short tag for UI / log dataset prefix
    channels:   list[Channel]
    port_path:  str | None                    = None  # latest discovered device path
    port:       serial.Serial | None          = None
    session:    Session | None                = None
    connecting: bool                          = False
    ch_id_map:  dict[int, Channel]            = field(default_factory=dict)
    buffers:    dict[str, _DisplayBuffer]     = field(default_factory=dict)


def _dataset_name(tag: str, name: str) -> str:
    return f'{tag}_{name}'


class Monitor:
    """Multi-board monitor. Discovers kvasir devices by USB VID/PID + serial,
    spawns one Session per configured board, and renders all channels in one window."""

    def __init__(self, config: Config) -> None:
        self._cfg    = config
        tags         = compute_tags(list(config.boards.keys()))
        self._boards: dict[str, _Board] = {
            serial: _Board(serial=serial, tag=tags[serial], channels=chans)
            for serial, chans in config.boards.items()
        }
        self._log:      object | None              = None
        self._datasets: dict[str, object]          = {}
        self._pending:  dict[str, list[np.ndarray]] = {}
        self._bytes_in    = 0
        self._samples_in  = 0
        self._t_stats     = time.monotonic()
        self._last_stats  = ''

    def on_stream_frame(self, board: _Board, frame: StreamDataFrame) -> None:
        """Test seam: dispatch one decoded STREAM_DATA frame onto board's buffers and log."""
        ch = board.ch_id_map.get(frame.channel_id)
        if ch is None:
            return
        raw = np.array(frame.samples, dtype=np.float32)
        eng = ch.convert(raw)
        buf = board.buffers.get(ch.name)
        if buf is not None:
            buf.push(eng)
        self._samples_in += len(eng)
        if self._log:
            key = _dataset_name(board.tag, ch.name)
            self._pending[key].append(eng)
            if sum(len(p) for p in self._pending[key]) >= LOG_CHUNK:
                self._flush_log()

    def run(self, gui: bool = True) -> None:
        if gui:
            self._run_gui()
        else:
            self._run_headless()

    def _open_log(self, path: str) -> None:
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
                ds.attrs['scale']  = ch.scale
                ds.attrs['offset'] = ch.offset
                ds.attrs['serial'] = board.serial
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
            # Drop stale paths if the OS reused them for a different (or absent) device.
        # Warn once about attached boards that aren't in the config.
        unknown = set(present) - set(self._boards)
        for s in unknown - self._warned_unknown:
            print(f'Ignoring unconfigured kvasir on {present[s]} (serial {s}).',
                  file=sys.stderr)
            self._warned_unknown.add(s)

    _warned_unknown: set[str] = set()  # class-level so it survives across run calls

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

        name_to_ch = {ch.channel: ch for ch in board.channels}
        board.ch_id_map = {
            ci.channel_id: name_to_ch[ci.name]
            for ci in session.device_info.channels
            if ci.name in name_to_ch
        }
        for ch in board.ch_id_map.values():
            if ch.name not in board.buffers:
                board.buffers[ch.name] = _DisplayBuffer(DISPLAY_POINTS, self._cfg.window)
        if self._log:
            for ch_id, ch in board.ch_id_map.items():
                rate = session.actual_rates.get(ch_id)
                key  = _dataset_name(board.tag, ch.name)
                if rate is not None and key in self._datasets:
                    self._datasets[key].attrs['sample_rate'] = rate

        board.port    = port
        board.session = session
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
            self._flush_log()
            self._log.close()
        for board in self._boards.values():
            self._disconnect(board)

    def _window_title(self) -> str:
        if not self._boards:
            return 'Kvasir Monitor — no boards configured'
        parts = []
        for board in self._boards.values():
            mark = '✓' if board.session is not None else '…'
            parts.append(f'[{board.tag}]{mark}')
        return 'Kvasir Monitor — ' + ' '.join(parts)

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
                        print(f'Lost [{board.tag}] on {board.port_path}, reconnecting…',
                              file=sys.stderr)
                        self._disconnect(board)

                if not read_any:
                    time.sleep(0.02)

                s = self._stats()
                if s and s != self._last_stats:
                    pass  # _stats() updates _last_stats; print on cadence below
                # Print stats line at most once per second.
                if s and now - getattr(self, '_t_print', 0.0) >= 1.0:
                    print(s, file=sys.stderr)
                    self._t_print = now
        except KeyboardInterrupt:
            pass
        finally:
            self._close()

    def _run_gui(self) -> None:
        import pyqtgraph as pg
        from pyqtgraph.Qt import QtCore

        if not self._boards:
            print('No boards in config; nothing to do.', file=sys.stderr)
            return
        if self._cfg.log:
            self._open_log(self._cfg.log)

        total_rows = sum(len(b.channels) for b in self._boards.values())

        pg.mkQApp('Kvasir Monitor')
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
            read_timer.stop()
            display_timer.stop()
            self._close()


def main() -> None:
    ap = argparse.ArgumentParser(description='Kvasir sensor capture monitor')
    ap.add_argument('config', nargs='?', default=str(DEFAULT_CONFIG),
                    help=f'YAML config file (default: {DEFAULT_CONFIG.name})')
    ap.add_argument('--no-gui', action='store_true',
                    help='headless logging, no display required')
    ap.add_argument('--output', metavar='FILE',
                    help='HDF5 output file (overrides config log)')
    args = ap.parse_args()

    cfg = load_config(args.config)
    if args.output:
        cfg.log = args.output

    Monitor(cfg).run(gui=not args.no_gui)


if __name__ == '__main__':
    main()

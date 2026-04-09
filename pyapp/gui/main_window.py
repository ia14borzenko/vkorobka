"""Главное окно: вкладки + журнал."""

from __future__ import annotations

import threading
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, scrolledtext, ttk

import numpy as np

from gui.session import AppSession
from gui.tabs import (
    ConnectionTab,
    DiagnosticTab,
    DisplayTab,
    SmartSpeakerTab,
    SpeakerTab,
    TextingTab,
    VoiceTab,
)
from vkorobka_client import VkorobkaClient
from voice_cli import STREAM_ID_MIC, parse_mic_stream_payload


class MicSignalPlotWindow:
    """Окно мониторинга входящего аудиосигнала микрофона."""

    def __init__(self, root: tk.Tk, session: AppSession):
        self.root = root
        self.session = session
        self.top = tk.Toplevel(root)
        self.top.title("График микрофона (входящий поток)")
        self.top.geometry("960x520")

        self._lock = threading.Lock()
        self._history_samples = np.array([], dtype=np.int32)
        self._history_threshold = np.array([], dtype=np.float32)
        self._sample_rate = 0
        self._bits = 0
        self._avg_abs = 0.0
        self._noise_floor_ema = 0.0
        self._tap_token = None
        self._history_limit = 240000  # до ~5 сек при 48kHz

        ctrl = ttk.LabelFrame(self.top, text="Настройки графика")
        ctrl.pack(fill=tk.X, padx=8, pady=(8, 2))
        ttk.Label(ctrl, text="Масштаб X").grid(row=0, column=0, padx=4, pady=2, sticky=tk.W)
        self.x_zoom_var = tk.StringVar(value="2.0")
        ttk.Entry(ctrl, textvariable=self.x_zoom_var, width=6).grid(row=0, column=1, padx=4, pady=2, sticky=tk.W)
        ttk.Label(ctrl, text="Полуразмах Y (отсчёты)").grid(row=0, column=2, padx=4, pady=2, sticky=tk.W)
        self.y_half_range_var = tk.StringVar(value="0")
        ttk.Entry(ctrl, textvariable=self.y_half_range_var, width=10).grid(row=0, column=3, padx=4, pady=2, sticky=tk.W)
        ttk.Label(ctrl, text="Отсчётов на экране").grid(row=0, column=4, padx=4, pady=2, sticky=tk.W)
        self.visible_samples_var = tk.StringVar(value="0")
        ttk.Entry(ctrl, textvariable=self.visible_samples_var, width=10).grid(row=0, column=5, padx=4, pady=2, sticky=tk.W)
        ttk.Label(ctrl, text="Точек отрисовки").grid(row=0, column=6, padx=4, pady=2, sticky=tk.W)
        self.max_points_var = tk.StringVar(value="3000")
        ttk.Entry(ctrl, textvariable=self.max_points_var, width=8).grid(row=0, column=7, padx=4, pady=2, sticky=tk.W)
        ttk.Label(ctrl, text="(0 = авто/без ограничения где применимо)").grid(
            row=1, column=0, columnspan=8, padx=4, pady=(0, 4), sticky=tk.W
        )

        info = ttk.Frame(self.top)
        info.pack(fill=tk.X, padx=8, pady=(4, 2))
        self.info_var = tk.StringVar(value="Нет данных. Запустите запись/поток микрофона.")
        ttk.Label(info, textvariable=self.info_var).pack(side=tk.LEFT)

        self.canvas = tk.Canvas(self.top, bg="white", highlightthickness=1, highlightbackground="#999")
        self.canvas.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        self._attach_stream_tap()
        self._schedule_redraw()
        self.top.protocol("WM_DELETE_WINDOW", self.close)

    def _attach_stream_tap(self) -> None:
        client = self.session.client
        if not client:
            raise RuntimeError("Сначала подключитесь на вкладке «Сеть».")
        self._tap_token = client.add_stream_tap_handler(STREAM_ID_MIC, self._on_stream_mic)

    def _on_stream_mic(self, message) -> None:
        payload = message.get("payload") or ""
        if not payload:
            return
        try:
            rate, block, bits = parse_mic_stream_payload(payload)
            arr = np.asarray(block, dtype=np.int32 if bits == 24 else np.int16).ravel()
            avg_abs = float(np.mean(np.abs(arr))) if arr.size else 0.0
            thr = self._calc_threshold(avg_abs)
            with self._lock:
                if self._history_samples.size == 0:
                    self._history_samples = arr.astype(np.int32, copy=False)
                    self._history_threshold = np.full(arr.shape[0], thr, dtype=np.float32)
                else:
                    self._history_samples = np.concatenate((self._history_samples, arr.astype(np.int32, copy=False)))
                    thr_append = np.full(arr.shape[0], thr, dtype=np.float32)
                    self._history_threshold = np.concatenate((self._history_threshold, thr_append))
                    if self._history_samples.size > self._history_limit:
                        self._history_samples = self._history_samples[-self._history_limit:]
                        self._history_threshold = self._history_threshold[-self._history_limit:]
                self._sample_rate = int(rate)
                self._bits = int(bits)
                self._avg_abs = avg_abs
        except Exception:
            return

    def _calc_threshold(self, level: float) -> float:
        base_thr = float(max(0.0, self.session.smart_silence_threshold))
        if not self.session.smart_silence_adaptive:
            self._noise_floor_ema = 0.0
            return base_thr
        alpha = max(0.001, min(0.5, float(self.session.smart_silence_noise_alpha)))
        mul = max(1.0, min(100.0, float(self.session.smart_silence_multiplier)))
        if self._noise_floor_ema <= 0.0:
            self._noise_floor_ema = level
        else:
            quiet_gate = self._noise_floor_ema * 1.5
            eff_alpha = alpha if level <= quiet_gate else alpha * 0.1
            self._noise_floor_ema = (1.0 - eff_alpha) * self._noise_floor_ema + eff_alpha * level
        return max(base_thr, self._noise_floor_ema * mul)

    def _schedule_redraw(self) -> None:
        if not self.top.winfo_exists():
            return
        self._redraw()
        self.top.after(100, self._schedule_redraw)

    def _redraw(self) -> None:
        with self._lock:
            samples = self._history_samples.copy()
            thresholds = self._history_threshold.copy()
            sr = self._sample_rate
            bits = self._bits
            avg_abs = self._avg_abs

        self.canvas.delete("all")
        w = max(200, self.canvas.winfo_width())
        h = max(180, self.canvas.winfo_height())

        left = 70
        right = w - 20
        top = 20
        bottom = h - 45

        self.canvas.create_line(left, top, left, bottom, fill="#333", width=2)  # Y
        self.canvas.create_line(left, bottom, right, bottom, fill="#333", width=2)  # X
        self.canvas.create_text((left + right) / 2, h - 18, text="Выборка (индекс)", fill="#222")
        self.canvas.create_text(22, (top + bottom) / 2, text="Амплитуда", angle=90, fill="#222")

        if samples.size == 0:
            self.info_var.set("Нет данных микрофона. Откройте поток записи, затем смотрите график.")
            return

        max_amp = 8388607 if bits == 24 else 32767
        n_total = samples.size
        x_zoom = self._safe_float(self.x_zoom_var.get(), 2.0, min_v=0.1, max_v=100.0)
        visible_override = self._safe_int(self.visible_samples_var.get(), 0, min_v=0, max_v=self._history_limit)
        if visible_override > 0:
            n_visible = min(n_total, visible_override)
        else:
            n_visible = max(1, int(round(n_total / x_zoom)))
        window = samples[-n_visible:]
        thr_window = thresholds[-n_visible:] if thresholds.size else np.array([], dtype=np.float32)

        max_points = self._safe_int(self.max_points_var.get(), 3000, min_v=0, max_v=100000)
        if max_points > 0 and n_visible > max_points:
            step = int(np.ceil(n_visible / float(max_points)))
            view = window[::step]
            thr_view = thr_window[::step] if thr_window.size else np.array([], dtype=np.float32)
        else:
            view = window
            thr_view = thr_window
            step = 1

        y_half = self._safe_int(self.y_half_range_var.get(), 0, min_v=0, max_v=max_amp)
        if y_half <= 0:
            y_half = max_amp
        tick_vals = [-y_half, -y_half // 2, 0, y_half // 2, y_half]
        for v in tick_vals:
            y = top + (1.0 - ((v + y_half) / (2.0 * y_half))) * (bottom - top)
            self.canvas.create_line(left - 5, y, left + 5, y, fill="#666")
            self.canvas.create_text(left - 8, y, text=str(int(v)), anchor=tk.E, fill="#333")

        x_ticks = 5
        for i in range(x_ticks + 1):
            x = left + i * (right - left) / x_ticks
            src_idx = (n_total - n_visible) + int((n_visible - 1) * i / x_ticks)
            self.canvas.create_line(x, bottom - 4, x, bottom + 4, fill="#666")
            self.canvas.create_text(x, bottom + 14, text=str(src_idx), fill="#333")

        if view.size > 1:
            pts = []
            denom = max(1, view.size - 1)
            for i, v in enumerate(view):
                x = left + (i / denom) * (right - left)
                vv = max(-y_half, min(y_half, float(v)))
                y = top + (1.0 - ((vv + y_half) / (2.0 * y_half))) * (bottom - top)
                pts.extend((x, y))
            self.canvas.create_line(*pts, fill="#0b66d0", width=1)

        if thr_view.size > 1:
            thr_pts = []
            denom_thr = max(1, thr_view.size - 1)
            for i, tv in enumerate(thr_view):
                x = left + (i / denom_thr) * (right - left)
                vv = max(-y_half, min(y_half, float(tv)))
                y = top + (1.0 - ((vv + y_half) / (2.0 * y_half))) * (bottom - top)
                thr_pts.extend((x, y))
            self.canvas.create_line(*thr_pts, fill="#d62828", width=2)
            self.canvas.create_text(right - 120, top + 12, text="Порог тишины", fill="#d62828", anchor=tk.W)

        dur_ms = (n_visible / float(sr) * 1000.0) if sr > 0 else 0.0
        self.info_var.set(
            f"sample_rate={sr} Hz, bits={bits}, history={n_total}, visible={n_visible}, "
            f"avg_abs={avg_abs:.1f}, порог={float(thr_view[-1]) if thr_view.size else 0.0:.1f}, "
            f"окно≈{dur_ms:.1f} мс, step={step}, y_half={y_half}"
        )

    @staticmethod
    def _safe_int(raw: str, default: int, *, min_v: int, max_v: int) -> int:
        try:
            v = int(raw.strip())
        except Exception:
            return default
        if v < min_v:
            return min_v
        if v > max_v:
            return max_v
        return v

    @staticmethod
    def _safe_float(raw: str, default: float, *, min_v: float, max_v: float) -> float:
        try:
            v = float(raw.strip())
        except Exception:
            return default
        if v < min_v:
            return min_v
        if v > max_v:
            return max_v
        return v

    def close(self) -> None:
        client = self.session.client
        if client and self._tap_token is not None:
            try:
                client.remove_stream_tap_handler(self._tap_token)
            except Exception:
                pass
        if self.top.winfo_exists():
            self.top.destroy()


def main() -> None:
    root = tk.Tk()
    root.title("VKOROBKA — управление ESP32-S3")
    root.minsize(800, 640)
    root.geometry("960x760")

    lf = ttk.LabelFrame(root, text="Журнал")
    log_widget = scrolledtext.ScrolledText(lf, height=11, wrap=tk.WORD)
    status_var = tk.StringVar(value="Готово.")
    log_file_path = Path(__file__).resolve().parents[1] / "gui_commands.log"
    log_file_lock = threading.Lock()

    def log(msg: str) -> None:
        ts = datetime.now().strftime("%H:%M:%S:%f")[:-3]
        line = f"{ts} {msg}"

        def append() -> None:
            log_widget.insert(tk.END, line + "\n")
            log_widget.see(tk.END)

        with log_file_lock:
            with log_file_path.open("a", encoding="utf-8") as fp:
                fp.write(line + "\n")

        root.after(0, append)

    session = AppSession(log)
    session.set_status_callback(lambda msg: root.after(0, lambda: status_var.set(msg)))

    def _safe_int(raw: str, default: int) -> int:
        try:
            return int(raw.strip())
        except Exception:
            return default

    def _safe_float(raw: str, default: float) -> float:
        try:
            return float(raw.strip())
        except Exception:
            return default

    content = ttk.Frame(root)
    content.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
    left = ttk.LabelFrame(content, text="Общие настройки (микрофон / динамик)")
    left.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 4))
    right = ttk.Frame(content)
    right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

    lrow = 0
    ttk.Label(left, text="Микрофон").grid(row=lrow, column=0, columnspan=4, sticky=tk.W, padx=4, pady=(4, 2))
    lrow += 1
    mic_rate_var = tk.StringVar(value=str(session.mic_rate_hz))
    mic_bits_var = tk.StringVar(value=str(session.mic_bits))
    mic_gain_var = tk.StringVar(value=str(session.mic_gain_db))
    mic_chunk_var = tk.StringVar(value=str(session.mic_chunk_samples))
    mic_record_gain_var = tk.StringVar(value=str(session.mic_record_gain_db))
    mic_mute_var = tk.BooleanVar(value=session.mic_mute)
    mic_clip_var = tk.BooleanVar(value=session.mic_clip)
    ttk.Label(left, text="rate_hz").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=mic_rate_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Label(left, text="bits").grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Combobox(left, textvariable=mic_bits_var, values=("16", "24"), width=6, state="readonly").grid(
        row=lrow, column=3, sticky=tk.W, padx=4, pady=2
    )
    lrow += 1
    ttk.Label(left, text="gain_db").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=mic_gain_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Label(left, text="chunk").grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=mic_chunk_var, width=8).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1
    ttk.Label(left, text="record_gain_db").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=mic_record_gain_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Checkbutton(left, text="mute", variable=mic_mute_var).grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Checkbutton(left, text="clip", variable=mic_clip_var).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1

    ttk.Separator(left, orient=tk.HORIZONTAL).grid(row=lrow, column=0, columnspan=4, sticky=tk.EW, padx=4, pady=4)
    lrow += 1
    ttk.Label(left, text="Динамик").grid(row=lrow, column=0, columnspan=4, sticky=tk.W, padx=4, pady=(2, 2))
    lrow += 1
    sp_chunk_var = tk.StringVar(value=str(session.speaker_chunk_samples))
    sp_pace_var = tk.StringVar(value=str(session.speaker_pace_factor))
    sp_timeout_var = tk.StringVar(value=str(session.speaker_command_timeout_s))
    sp_rate_var = tk.StringVar(value=str(session.speaker_dyn_rate_hz))
    sp_gain_var = tk.StringVar(value=str(session.speaker_dyn_gain_db))
    sp_no_pace_var = tk.BooleanVar(value=session.speaker_no_pace)
    sp_skip_dyn_set_var = tk.BooleanVar(value=session.speaker_skip_dyn_set)
    sp_dyn_mute_var = tk.BooleanVar(value=session.speaker_dyn_mute)
    sp_dyn_clip_var = tk.BooleanVar(value=session.speaker_dyn_clip)
    sp_flow_control_var = tk.BooleanVar(value=session.speaker_flow_control)
    sp_adaptive_pace_var = tk.BooleanVar(value=session.speaker_adaptive_pace)
    sp_ack_timeout_var = tk.StringVar(value=str(session.speaker_max_ack_wait_s))
    sp_flow_window_var = tk.StringVar(value=str(session.speaker_flow_window))
    ttk.Label(left, text="chunk").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=sp_chunk_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Label(left, text="pace_factor").grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=sp_pace_var, width=8).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1
    ttk.Label(left, text="timeout_s").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=sp_timeout_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Label(left, text="dyn_rate").grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Combobox(
        left,
        textvariable=sp_rate_var,
        values=[str(x) for x in sorted(VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ)],
        width=8,
        state="readonly",
    ).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1
    ttk.Label(left, text="dyn_gain_db").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=sp_gain_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Checkbutton(left, text="no_pace", variable=sp_no_pace_var).grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Checkbutton(left, text="skip_dyn_set", variable=sp_skip_dyn_set_var).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1
    ttk.Checkbutton(left, text="dyn_mute", variable=sp_dyn_mute_var).grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Checkbutton(left, text="dyn_clip", variable=sp_dyn_clip_var).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1
    ttk.Label(left, text="ack_timeout_s").grid(row=lrow, column=0, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=sp_ack_timeout_var, width=8).grid(row=lrow, column=1, sticky=tk.W, padx=4, pady=2)
    ttk.Label(left, text="flow_window").grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Entry(left, textvariable=sp_flow_window_var, width=8).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)
    lrow += 1
    ttk.Checkbutton(left, text="flow_control", variable=sp_flow_control_var).grid(row=lrow, column=2, sticky=tk.W, padx=4, pady=2)
    ttk.Checkbutton(left, text="adaptive_pace", variable=sp_adaptive_pace_var).grid(row=lrow, column=3, sticky=tk.W, padx=4, pady=2)

    def _sync_shared_settings(*_args) -> None:
        session.mic_rate_hz = max(8000, min(96000, _safe_int(mic_rate_var.get(), session.mic_rate_hz)))
        bits = _safe_int(mic_bits_var.get(), session.mic_bits)
        session.mic_bits = 24 if bits == 24 else 16
        session.mic_gain_db = _safe_float(mic_gain_var.get(), session.mic_gain_db)
        session.mic_chunk_samples = max(64, min(512, _safe_int(mic_chunk_var.get(), session.mic_chunk_samples)))
        session.mic_record_gain_db = _safe_float(mic_record_gain_var.get(), session.mic_record_gain_db)
        session.mic_mute = bool(mic_mute_var.get())
        session.mic_clip = bool(mic_clip_var.get())
        session.speaker_chunk_samples = max(1, min(512, _safe_int(sp_chunk_var.get(), session.speaker_chunk_samples)))
        session.speaker_pace_factor = _safe_float(sp_pace_var.get(), session.speaker_pace_factor)
        session.speaker_command_timeout_s = max(0.5, _safe_float(sp_timeout_var.get(), session.speaker_command_timeout_s))
        rate = _safe_int(sp_rate_var.get(), session.speaker_dyn_rate_hz)
        if rate in VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ:
            session.speaker_dyn_rate_hz = rate
        session.speaker_dyn_gain_db = _safe_float(sp_gain_var.get(), session.speaker_dyn_gain_db)
        session.speaker_no_pace = bool(sp_no_pace_var.get())
        session.speaker_skip_dyn_set = bool(sp_skip_dyn_set_var.get())
        session.speaker_dyn_mute = bool(sp_dyn_mute_var.get())
        session.speaker_dyn_clip = bool(sp_dyn_clip_var.get())
        session.speaker_flow_control = bool(sp_flow_control_var.get())
        session.speaker_adaptive_pace = bool(sp_adaptive_pace_var.get())
        session.speaker_max_ack_wait_s = max(0.001, min(2.0, _safe_float(sp_ack_timeout_var.get(), session.speaker_max_ack_wait_s)))
        session.speaker_flow_window = max(1, min(128, _safe_int(sp_flow_window_var.get(), session.speaker_flow_window)))

    for v in (mic_rate_var, mic_bits_var, mic_gain_var, mic_chunk_var, mic_record_gain_var, sp_chunk_var, sp_pace_var, sp_timeout_var, sp_rate_var, sp_gain_var, sp_ack_timeout_var, sp_flow_window_var):
        v.trace_add("write", _sync_shared_settings)
    for v in (mic_mute_var, mic_clip_var, sp_no_pace_var, sp_skip_dyn_set_var, sp_dyn_mute_var, sp_dyn_clip_var, sp_flow_control_var, sp_adaptive_pace_var):
        v.trace_add("write", _sync_shared_settings)
    _sync_shared_settings()

    nb = ttk.Notebook(right)
    nb.pack(fill=tk.BOTH, expand=True)

    nb.add(ConnectionTab(nb, session, root), text="Сеть")
    nb.add(DisplayTab(nb, session, root), text="Дисплей")
    nb.add(TextingTab(nb, session, root), text="Текстинг")
    nb.add(VoiceTab(nb, session, root), text="Микрофон")
    nb.add(SpeakerTab(nb, session, root), text="Динамик")
    nb.add(SmartSpeakerTab(nb, session, root), text="Умная колонка")
    nb.add(DiagnosticTab(nb, session, root), text="Диагностика")

    log_tools = ttk.Frame(lf)
    log_tools.pack(fill=tk.X, padx=4, pady=(4, 0))

    def copy_main_log() -> None:
        try:
            text = log_widget.get("1.0", tk.END).strip()
            root.clipboard_clear()
            root.clipboard_append(text)
            status_var.set("Журнал скопирован в буфер обмена.")
        except Exception as e:
            messagebox.showerror("Журнал", str(e))

    ttk.Button(log_tools, text="Скопировать журнал", command=copy_main_log).pack(side=tk.LEFT)

    lf.pack(fill=tk.BOTH, expand=False, padx=4, pady=(0, 4), side=tk.BOTTOM)
    log_widget.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
    status_bar = ttk.Label(root, textvariable=status_var, anchor=tk.W, relief=tk.SUNKEN)
    status_bar.pack(fill=tk.X, side=tk.BOTTOM, padx=4, pady=(0, 4))

    def on_close() -> None:
        session.disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)

    help_text = (
        "Справка по вкладкам приложения VKOROBKA\n\n"
        "Общий принцип работы:\n"
        "- Сначала подключение на вкладке «Сеть», затем работа с функциональными вкладками.\n"
        "- Общие аудиопараметры (микрофон/динамик) задаются в левой панели окна и используются вкладками «Микрофон», «Динамик» и «Умная колонка».\n"
        "- Длительные операции выполняются в фоне; ход выполнения смотрите в «Журнале».\n\n"
        "1) Сеть\n"
        "- Назначение: подключение GUI к win-x64 шлюзу по UDP.\n"
        "- Что настраивается: host/port, timeout, уровень логирования UDP, destination.\n"
        "- Типовой порядок: сначала подключиться здесь, затем работать в остальных вкладках.\n\n"
        "2) Дисплей\n"
        "- Назначение: отправка полноэкранного кадра на LCD ESP32.\n"
        "- Как работает: изображение приводится к размеру дисплея и отправляется чанками.\n"
        "- Важный параметр: chunk_rows (высота чанка в строках); больше значение — меньше накладных расходов, но выше нагрузка на буфер.\n\n"
        "3) Текстинг\n"
        "- Назначение: печать текста в заданном прямоугольном поле на дисплее.\n"
        "- Что настраивается: шрифт, геометрия поля, высота символа, межстрочный интервал, скорость печати.\n"
        "- Кнопки: очистка поля и отправка текста. При необходимости можно регенерировать кэш символов.\n\n"
        "4) Микрофон\n"
        "- Назначение: ручной запуск записи с микрофона ESP32 (voice.on/voice.off).\n"
        "- Что настраивается: частота, битность, gain, размер чанка, mute/clip и post-gain для файла.\n"
        "- Результат: сохраняются WAV/FLAC/TSV; добавлены кнопки локального прослушивания WAV.\n\n"
        "5) Динамик\n"
        "- Назначение: отправка аудиофайла на динамик ESP32.\n"
        "- Что настраивается: chunk_samples, pace_factor/no-pace, timeout, параметры dyn.set, flow_control/adaptive_pace, ack_timeout и flow_window.\n"
        "- Кнопка запускает последовательность dyn.set (опц) -> dyn.on -> PCM stream -> dyn.off.\n\n"
        "6) Умная колонка\n"
        "- Назначение: автоматический цикл сценария колонки.\n"
        "- Состояния: ожидание wake-фразы -> запись запроса -> ожидание back-end -> вывод ответа -> возврат в ожидание.\n"
        "- Режимы остановки записи: по тишине (порог + время ожидания) или по внешней команде.\n"
        "- Вывод ответа: почти параллельный запуск аудио и текста; скорость текста авто (по длительности аудио) или ручная.\n"
        "- Тестовый режим: ручная генерация wake/stop/response событий без реального back-end.\n\n"
        "7) Диагностика\n"
        "- Назначение: сервисные проверки и отладочные действия.\n"
        "- Используйте эту вкладку для тестовых команд и контроля состояния компонентов.\n"
        "- Кнопки тестируют зеркальную обработку JPG как в CLI `main.py`.\n\n"
        "Контекстное меню:\n"
        "- ПКМ в любом месте окна открывает меню с пунктами:\n"
        "  • «Справка по вкладкам» (это окно)\n"
        "  • «График микрофона» (живой мониторинг входящего STREAM_ID_MIC)\n\n"
        "Полезно:\n"
        "- Кнопка «Скопировать журнал» копирует весь текст лога в буфер обмена для отчёта/диагностики."
    )

    def open_help() -> None:
        w = tk.Toplevel(root)
        w.title("Справка по вкладкам")
        w.geometry("900x700")
        text = scrolledtext.ScrolledText(w, wrap=tk.WORD)
        text.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        text.insert(tk.END, help_text)
        text.configure(state=tk.DISABLED)

    mic_plot_window = {"obj": None}

    def open_mic_plot() -> None:
        if not session.client:
            messagebox.showwarning("График микрофона", "Сначала подключитесь на вкладке «Сеть».")
            return
        obj = mic_plot_window.get("obj")
        if obj and obj.top.winfo_exists():
            obj.top.lift()
            obj.top.focus_force()
            return
        try:
            mic_plot_window["obj"] = MicSignalPlotWindow(root, session)
            session.log("[ui] Открыто окно графика микрофона")
        except Exception as e:
            messagebox.showerror("График микрофона", str(e))

    menu = tk.Menu(root, tearoff=0)
    menu.add_command(label="Справка по вкладкам", command=open_help)
    menu.add_command(label="График микрофона", command=open_mic_plot)
    menu.add_separator()
    menu.add_command(label="Закрыть меню")

    def show_context_menu(event) -> None:
        menu.tk_popup(event.x_root, event.y_root)

    root.bind_all("<Button-3>", show_context_menu)

    log("VKOROBKA GUI: подключитесь на вкладке «Сеть», затем используйте остальные вкладки.")
    root.mainloop()

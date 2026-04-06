"""Главное окно: вкладки + журнал."""

from __future__ import annotations

import threading
import tkinter as tk
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
        self._sample_rate = 0
        self._bits = 0
        self._avg_abs = 0.0
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
            with self._lock:
                if self._history_samples.size == 0:
                    self._history_samples = arr.astype(np.int32, copy=False)
                else:
                    self._history_samples = np.concatenate((self._history_samples, arr.astype(np.int32, copy=False)))
                    if self._history_samples.size > self._history_limit:
                        self._history_samples = self._history_samples[-self._history_limit :]
                self._sample_rate = int(rate)
                self._bits = int(bits)
                self._avg_abs = avg_abs
        except Exception:
            return

    def _schedule_redraw(self) -> None:
        if not self.top.winfo_exists():
            return
        self._redraw()
        self.top.after(100, self._schedule_redraw)

    def _redraw(self) -> None:
        with self._lock:
            samples = self._history_samples.copy()
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

        max_points = self._safe_int(self.max_points_var.get(), 3000, min_v=0, max_v=100000)
        if max_points > 0 and n_visible > max_points:
            step = int(np.ceil(n_visible / float(max_points)))
            view = window[::step]
        else:
            view = window
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

        dur_ms = (n_visible / float(sr) * 1000.0) if sr > 0 else 0.0
        self.info_var.set(
            f"sample_rate={sr} Hz, bits={bits}, history={n_total}, visible={n_visible}, "
            f"avg_abs={avg_abs:.1f}, окно≈{dur_ms:.1f} мс, step={step}, y_half={y_half}"
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

    def log(msg: str) -> None:
        def append() -> None:
            log_widget.insert(tk.END, msg + "\n")
            log_widget.see(tk.END)

        root.after(0, append)

    session = AppSession(log)
    session.set_status_callback(lambda msg: root.after(0, lambda: status_var.set(msg)))

    nb = ttk.Notebook(root)
    nb.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

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
        "- Что настраивается: chunk_samples, pace_factor/no-pace, timeout, параметры dyn.set.\n"
        "- Кнопка запускает последовательность dyn.set (опц) -> dyn.on -> PCM stream -> dyn.off.\n\n"
        "6) Умная колонка\n"
        "- Назначение: автоматический цикл сценария колонки.\n"
        "- Состояния: ожидание wake-фразы -> запись запроса -> ожидание back-end -> вывод ответа -> возврат в ожидание.\n"
        "- Режимы остановки записи: по тишине (порог + время ожидания) или по внешней команде.\n"
        "- Вывод ответа: почти параллельный запуск аудио и текста; скорость текста авто (по длительности аудио) или ручная.\n"
        "- Тестовый режим: ручная генерация wake/stop/response событий без реального back-end.\n\n"
        "7) Диагностика\n"
        "- Назначение: сервисные проверки и отладочные действия.\n"
        "- Используйте эту вкладку для тестовых команд и контроля состояния компонентов.\n\n"
        "Контекстное меню:\n"
        "- ПКМ в любом месте окна открывает меню с пунктом этой справки."
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

"""Вкладка Smart Speaker: оркестрация wake->capture->response->idle."""

from __future__ import annotations

import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk

from gui.backend_contract import MockBackendAdapter, ResponsePayload
from gui.smart_speaker_orchestrator import SmartSpeakerOrchestrator, TextingConfig, VoiceConfig


class SmartSpeakerTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root
        self.backend = MockBackendAdapter()
        self.orchestrator = SmartSpeakerOrchestrator(
            root=root,
            client_getter=session.require_client,
            destination_getter=lambda: session.destination,
            log=self._log,
            backend_adapter=self.backend,
            texting_cfg_getter=self._collect_texting_cfg,
            voice_cfg_getter=self._collect_voice_cfg,
            state_changed=self._on_state_changed,
        )

        self.state_var = tk.StringVar(value="Остановлен")
        self.stop_mode_var = tk.StringVar(value="silence")
        self.silence_threshold_var = tk.StringVar(value="6000")
        self.silence_wait_var = tk.StringVar(value="3.0")
        self.text_mode_var = tk.StringVar(value="auto")
        self.manual_cps_var = tk.StringVar(value="20")
        self.auto_min_cps_var = tk.StringVar(value="8")
        self.auto_max_cps_var = tk.StringVar(value="30")
        self.font_var = tk.StringVar(value="fonts/font.ttf")

        self.field_x_var = tk.StringVar(value="52")
        self.field_y_var = tk.StringVar(value="160")
        self.field_w_var = tk.StringVar(value="380")
        self.field_h_var = tk.StringVar(value="120")
        self.char_h_var = tk.StringVar(value="14")
        self.line_sp_var = tk.StringVar(value="2")

        self.audio_path_var = tk.StringVar(value="")
        self._build_ui()

    def _build_ui(self) -> None:
        r = 0
        sf = ttk.LabelFrame(self, text="Сценарий умной колонки")
        sf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        ttk.Label(sf, text="Состояние:").grid(row=0, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Label(sf, textvariable=self.state_var).grid(row=0, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Button(sf, text="Старт", command=self._start).grid(row=0, column=2, padx=4, pady=2)
        ttk.Button(sf, text="Стоп", command=self._stop).grid(row=0, column=3, padx=4, pady=2)
        r += 1

        vf = ttk.LabelFrame(self, text="Завершение записи запроса")
        vf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        ttk.Radiobutton(vf, text="По тишине", value="silence", variable=self.stop_mode_var).grid(
            row=0, column=0, sticky=tk.W, padx=4, pady=2
        )
        ttk.Radiobutton(vf, text="По внешней команде", value="external", variable=self.stop_mode_var).grid(
            row=0, column=1, sticky=tk.W, padx=4, pady=2
        )
        ttk.Label(vf, text="Порог среднего уровня").grid(row=1, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(vf, textvariable=self.silence_threshold_var, width=10).grid(row=1, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Label(vf, text="Ожидание тишины, сек").grid(row=1, column=2, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(vf, textvariable=self.silence_wait_var, width=10).grid(row=1, column=3, sticky=tk.W, padx=4, pady=2)
        r += 1

        tf = ttk.LabelFrame(self, text="Скорость печати текста")
        tf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        ttk.Radiobutton(tf, text="Авто (по длительности аудио)", value="auto", variable=self.text_mode_var).grid(
            row=0, column=0, sticky=tk.W, padx=4, pady=2
        )
        ttk.Radiobutton(tf, text="Ручная", value="manual", variable=self.text_mode_var).grid(
            row=0, column=1, sticky=tk.W, padx=4, pady=2
        )
        ttk.Label(tf, text="Ручная скорость (симв/с)").grid(row=1, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(tf, textvariable=self.manual_cps_var, width=8).grid(row=1, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Label(tf, text="Авто min/max (симв/с)").grid(row=1, column=2, sticky=tk.W, padx=4, pady=2)
        af = ttk.Frame(tf)
        af.grid(row=1, column=3, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(af, textvariable=self.auto_min_cps_var, width=6).pack(side=tk.LEFT, padx=(0, 2))
        ttk.Entry(af, textvariable=self.auto_max_cps_var, width=6).pack(side=tk.LEFT, padx=(2, 0))
        r += 1

        ff = ttk.LabelFrame(self, text="Параметры текстового поля")
        ff.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        ttk.Label(ff, text="Шрифт").grid(row=0, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(ff, textvariable=self.font_var, width=32).grid(row=0, column=1, sticky=tk.EW, padx=4, pady=2)
        ttk.Button(ff, text="Обзор…", command=self._browse_font).grid(row=0, column=2, padx=4, pady=2)
        ttk.Label(ff, text="x/y/w/h").grid(row=1, column=0, sticky=tk.W, padx=4, pady=2)
        xywh = ttk.Frame(ff)
        xywh.grid(row=1, column=1, sticky=tk.W, padx=4, pady=2)
        for var in (self.field_x_var, self.field_y_var, self.field_w_var, self.field_h_var):
            ttk.Entry(xywh, textvariable=var, width=6).pack(side=tk.LEFT, padx=2)
        ttk.Label(ff, text="char_h/line_sp").grid(row=1, column=2, sticky=tk.W, padx=4, pady=2)
        hs = ttk.Frame(ff)
        hs.grid(row=1, column=3, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(hs, textvariable=self.char_h_var, width=6).pack(side=tk.LEFT, padx=2)
        ttk.Entry(hs, textvariable=self.line_sp_var, width=6).pack(side=tk.LEFT, padx=2)
        ff.columnconfigure(1, weight=1)
        r += 1

        testf = ttk.LabelFrame(self, text="Тестовый режим (имитация back-end)")
        testf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        ttk.Button(testf, text="Событие: wake detected", command=self._simulate_wake).grid(row=0, column=0, padx=4, pady=2)
        ttk.Button(testf, text="Событие: остановить запись", command=self._simulate_stop).grid(row=0, column=1, padx=4, pady=2)
        ttk.Label(testf, text="Аудио ответа").grid(row=1, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(testf, textvariable=self.audio_path_var, width=40).grid(row=1, column=1, columnspan=2, sticky=tk.EW, padx=4, pady=2)
        ttk.Button(testf, text="Обзор…", command=self._browse_audio).grid(row=1, column=3, padx=4, pady=2)
        ttk.Label(testf, text="Текст ответа").grid(row=2, column=0, sticky=tk.NW, padx=4, pady=2)
        self.text_w = scrolledtext.ScrolledText(testf, width=52, height=6, wrap=tk.WORD)
        self.text_w.grid(row=2, column=1, columnspan=3, sticky=tk.EW, padx=4, pady=2)
        bf = ttk.Frame(testf)
        bf.grid(row=3, column=0, columnspan=4, sticky=tk.W, padx=4, pady=4)
        ttk.Button(bf, text="Отправить mock-ответ", command=self._simulate_response).pack(side=tk.LEFT, padx=4)
        ttk.Button(bf, text="Сценарий: короткий", command=self._scenario_short).pack(side=tk.LEFT, padx=4)
        ttk.Button(bf, text="Сценарий: только текст", command=self._scenario_text_only).pack(side=tk.LEFT, padx=4)
        ttk.Button(bf, text="Сценарий: только аудио", command=self._scenario_audio_only).pack(side=tk.LEFT, padx=4)
        testf.columnconfigure(1, weight=1)
        r += 1

        lf = ttk.LabelFrame(self, text="Журнал умной колонки")
        lf.grid(row=r, column=0, columnspan=3, sticky=tk.NSEW, padx=4, pady=4)
        lbf = ttk.Frame(lf)
        lbf.pack(fill=tk.X, padx=4, pady=4)
        ttk.Button(lbf, text="Скопировать лог", command=self._copy_log).pack(side=tk.LEFT)
        self.local_log = scrolledtext.ScrolledText(lf, width=80, height=8, wrap=tk.WORD)
        self.local_log.pack(fill=tk.BOTH, expand=True, padx=4, pady=(0, 4))
        self.local_log.configure(state=tk.DISABLED)
        r += 1

        self.columnconfigure(1, weight=1)
        self.rowconfigure(r - 1, weight=1)

    def _browse_font(self) -> None:
        p = filedialog.askopenfilename(title="Шрифт", filetypes=[("Шрифты", "*.ttf *.otf"), ("Все файлы", "*.*")])
        if p:
            self.font_var.set(p)

    def _browse_audio(self) -> None:
        p = filedialog.askopenfilename(title="Аудио", filetypes=[("Аудио", "*.wav *.flac *.mp3 *.ogg"), ("Все файлы", "*.*")])
        if p:
            self.audio_path_var.set(p)

    def _i(self, var: tk.StringVar, default: int) -> int:
        try:
            return int(var.get().strip())
        except ValueError:
            return default

    def _f(self, var: tk.StringVar, default: float) -> float:
        try:
            return float(var.get().strip())
        except ValueError:
            return default

    def _collect_voice_cfg(self) -> VoiceConfig:
        return VoiceConfig(
            stop_mode=self.stop_mode_var.get().strip() or "silence",
            silence_threshold=self._f(self.silence_threshold_var, 6000.0),
            silence_seconds=self._f(self.silence_wait_var, 3.0),
        )

    def _collect_texting_cfg(self) -> TextingConfig:
        auto_mode = self.text_mode_var.get() == "auto"
        return TextingConfig(
            font_path=self.font_var.get().strip() or "fonts/font.ttf",
            field_x=self._i(self.field_x_var, 52),
            field_y=self._i(self.field_y_var, 160),
            field_width=self._i(self.field_w_var, 380),
            field_height=self._i(self.field_h_var, 120),
            char_height=self._i(self.char_h_var, 14),
            line_spacing=self._i(self.line_sp_var, 2),
            manual_chars_per_sec=self._f(self.manual_cps_var, 20.0),
            auto_speed=auto_mode,
            min_auto_cps=self._f(self.auto_min_cps_var, 8.0),
            max_auto_cps=self._f(self.auto_max_cps_var, 30.0),
        )

    def _on_state_changed(self, state: str) -> None:
        ru = {
            SmartSpeakerOrchestrator.STATE_IDLE: "Ожидание wake-фразы",
            SmartSpeakerOrchestrator.STATE_CAPTURE: "Запись запроса",
            SmartSpeakerOrchestrator.STATE_WAIT_BACKEND: "Ожидание ответа back-end",
            SmartSpeakerOrchestrator.STATE_RENDER: "Вывод ответа",
            SmartSpeakerOrchestrator.STATE_STOPPED: "Остановлен",
        }
        self.state_var.set(ru.get(state, state))

    def _start(self) -> None:
        try:
            self.session.require_client()
            self.orchestrator.start()
        except Exception as e:
            messagebox.showerror("Smart Speaker", str(e))

    def _stop(self) -> None:
        self.orchestrator.stop()

    def _simulate_wake(self) -> None:
        self.backend.simulate_wake_detected()

    def _simulate_stop(self) -> None:
        self.backend.simulate_stop_capture()

    def _simulate_response(self) -> None:
        text = self.text_w.get("1.0", tk.END).rstrip("\n")
        audio_path = self.audio_path_var.get().strip()
        payload = ResponsePayload(
            audio_path=Path(audio_path) if audio_path else None,
            text=text,
            display_asset=None,
            is_animation=False,
        )
        self.backend.simulate_response_ready(payload)

    def _scenario_short(self) -> None:
        self.text_w.delete("1.0", tk.END)
        self.text_w.insert(tk.END, "Привет! Я готова выполнить ваш запрос.")
        self._simulate_response()

    def _scenario_text_only(self) -> None:
        self.audio_path_var.set("")
        self.text_w.delete("1.0", tk.END)
        self.text_w.insert(tk.END, "Это тестовый ответ только с текстом без звука.")
        self._simulate_response()

    def _scenario_audio_only(self) -> None:
        self.text_w.delete("1.0", tk.END)
        self._simulate_response()

    def _log(self, msg: str) -> None:
        self.session.log(msg)

        def append() -> None:
            self.local_log.configure(state=tk.NORMAL)
            self.local_log.insert(tk.END, msg + "\n")
            self.local_log.see(tk.END)
            self.local_log.configure(state=tk.DISABLED)

        self.root.after(0, append)

    def _copy_log(self) -> None:
        try:
            text = self.local_log.get("1.0", tk.END).strip()
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            messagebox.showinfo("Журнал", "Лог вкладки скопирован в буфер обмена.")
        except Exception as e:
            messagebox.showerror("Журнал", str(e))

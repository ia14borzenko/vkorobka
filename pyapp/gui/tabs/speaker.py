"""Вкладка: воспроизведение на динамике ESP32."""

from __future__ import annotations

import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from gui.threading_utils import run_in_thread
from vkorobka_client import VkorobkaClient


class SpeakerTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root

        r = 0
        ttk.Label(self, text="Аудиофайл:").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.path_var = tk.StringVar(value="")
        ttk.Entry(self, textvariable=self.path_var, width=48).grid(row=r, column=1, sticky=tk.EW, padx=4, pady=2)
        ttk.Button(self, text="Обзор…", command=self._browse).grid(row=r, column=2, padx=4, pady=2)
        r += 1

        f = ttk.LabelFrame(self, text="Параметры потока")
        f.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        rr = 0
        ttk.Label(f, text="chunk_samples (1..512)").grid(row=rr, column=0, sticky=tk.W, padx=4, pady=2)
        self.chunk_var = tk.StringVar(value="512")
        ttk.Entry(f, textvariable=self.chunk_var, width=8).grid(row=rr, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Label(f, text="pace_factor").grid(row=rr, column=2, sticky=tk.W, padx=4, pady=2)
        self.pace_factor_var = tk.StringVar(value="0.97")
        ttk.Entry(f, textvariable=self.pace_factor_var, width=8).grid(row=rr, column=3, sticky=tk.W, padx=4, pady=2)
        rr += 1
        self.no_pace_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text="Без паузы между чанками (--no-pace)", variable=self.no_pace_var).grid(
            row=rr, column=0, columnspan=2, sticky=tk.W, padx=4, pady=2
        )
        ttk.Label(f, text="command_timeout (с)").grid(row=rr, column=2, sticky=tk.W, padx=4, pady=2)
        self.cmd_to_var = tk.StringVar(value="4.0")
        ttk.Entry(f, textvariable=self.cmd_to_var, width=8).grid(row=rr, column=3, sticky=tk.W, padx=4, pady=2)
        rr += 1

        df = ttk.LabelFrame(self, text="dyn.set")
        df.grid(row=r + 1, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        rates = sorted(VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ)
        ttk.Label(df, text="dyn_rate_hz").grid(row=0, column=0, sticky=tk.W, padx=4, pady=2)
        self.rate_var = tk.StringVar(value=str(VkorobkaClient.DYN_PCM_SAMPLE_RATE_HZ))
        ttk.Combobox(df, textvariable=self.rate_var, values=[str(x) for x in rates], width=10).grid(
            row=0, column=1, sticky=tk.W, padx=4, pady=2
        )
        ttk.Label(df, text="dyn_gain_db").grid(row=0, column=2, sticky=tk.W, padx=4, pady=2)
        self.gain_var = tk.StringVar(value="0.0")
        ttk.Entry(df, textvariable=self.gain_var, width=8).grid(row=0, column=3, sticky=tk.W, padx=4, pady=2)
        self.skip_dyn_set_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(df, text="Не отправлять dyn.set", variable=self.skip_dyn_set_var).grid(
            row=1, column=0, columnspan=2, sticky=tk.W, padx=4, pady=2
        )
        self.dyn_mute_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(df, text="mute", variable=self.dyn_mute_var).grid(row=1, column=2, sticky=tk.W, padx=4, pady=2)
        self.dyn_clip_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(df, text="clip", variable=self.dyn_clip_var).grid(row=1, column=3, sticky=tk.W, padx=4, pady=2)
        r += 2

        ttk.Button(self, text="Воспроизвести на ESP32", command=self._play).grid(
            row=r, column=0, columnspan=3, pady=12
        )
        self.columnconfigure(1, weight=1)

    def _browse(self) -> None:
        p = filedialog.askopenfilename(
            title="Аудио",
            filetypes=[
                ("Аудио", "*.wav *.flac *.ogg *.mp3"),
                ("Все файлы", "*.*"),
            ],
        )
        if p:
            self.path_var.set(p)

    def _play(self) -> None:
        try:
            self.session.require_client()
        except RuntimeError as e:
            messagebox.showwarning("Динамик", str(e))
            return
        path = self.path_var.get().strip()
        if not path or not Path(path).is_file():
            messagebox.showwarning("Динамик", "Укажите существующий файл.")
            return
        try:
            chunk = int(self.chunk_var.get().strip())
            pace_factor = float(self.pace_factor_var.get().strip())
            cmd_to = float(self.cmd_to_var.get().strip())
            dyn_rate = int(self.rate_var.get().strip())
            dyn_gain = float(self.gain_var.get().strip())
        except ValueError:
            messagebox.showwarning("Динамик", "Некорректные числовые поля.")
            return
        if not 1 <= chunk <= 512:
            messagebox.showwarning("Динамик", "chunk_samples: 1..512")
            return
        if dyn_rate not in VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ:
            messagebox.showwarning(
                "Динамик",
                f"Частота должна быть из списка: {sorted(VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ)}",
            )
            return

        no_pace = self.no_pace_var.get()
        skip_set = self.skip_dyn_set_var.get()

        def work():
            c = self.session.require_client()
            return c.play_audio_file_to_esp32_dyn(
                path,
                chunk_samples=chunk,
                pace=not no_pace,
                pace_factor=pace_factor,
                command_timeout=cmd_to,
                dyn_rate_hz=dyn_rate,
                dyn_bits=16,
                dyn_gain_db=dyn_gain,
                dyn_mute=self.dyn_mute_var.get(),
                dyn_clip=self.dyn_clip_var.get(),
                send_dyn_set=not skip_set,
            )

        self.session.log("[speaker] Воспроизведение…")

        def ok(ok_flag: bool):
            if ok_flag:
                self.session.log("[speaker] Готово")
                self.session.status("Динамик: воспроизведение завершено.")
            else:
                self.session.log("[speaker] Ошибка или таймаут")
                messagebox.showwarning("Динамик", "Воспроизведение не завершилось успешно.")

        def err(e):
            self.session.log(f"[speaker] {e}")
            messagebox.showerror("Динамик", str(e))

        run_in_thread(self.root, work, ok, err)

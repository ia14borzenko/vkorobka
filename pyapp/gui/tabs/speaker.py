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

        ttk.Label(
            self,
            text="Параметры динамика берутся из левой панели общих настроек.",
        ).grid(row=r, column=0, columnspan=3, sticky=tk.W, padx=4, pady=(2, 8))
        r += 1

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
        chunk = int(self.session.speaker_chunk_samples)
        pace_factor = float(self.session.speaker_pace_factor)
        cmd_to = float(self.session.speaker_command_timeout_s)
        dyn_rate = int(self.session.speaker_dyn_rate_hz)
        dyn_gain = float(self.session.speaker_dyn_gain_db)
        if not 1 <= chunk <= 512:
            messagebox.showwarning("Динамик", "chunk_samples: 1..512")
            return
        if dyn_rate not in VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ:
            messagebox.showwarning(
                "Динамик",
                f"Частота должна быть из списка: {sorted(VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ)}",
            )
            return

        no_pace = self.session.speaker_no_pace
        skip_set = self.session.speaker_skip_dyn_set

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
                dyn_mute=self.session.speaker_dyn_mute,
                dyn_clip=self.session.speaker_dyn_clip,
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

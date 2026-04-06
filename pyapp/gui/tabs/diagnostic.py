"""Вкладка: зеркальный тест JPG (win / esp32) как в main.py."""

from __future__ import annotations

import tkinter as tk
from tkinter import messagebox, ttk

from gui.threading_utils import run_in_thread
from image_utils import generate_test_image, verify_image_integrity


class DiagnosticTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root

        ttk.Label(self, text="Размер тестового JPG (КБ):").grid(row=0, column=0, sticky=tk.W, padx=4, pady=4)
        self.size_kb_var = tk.StringVar(value="4")
        ttk.Entry(self, textvariable=self.size_kb_var, width=8).grid(row=0, column=1, sticky=tk.W, padx=4, pady=4)

        bf = ttk.Frame(self)
        bf.grid(row=1, column=0, columnspan=2, pady=12)
        ttk.Button(bf, text="Тест WIN (win-x64)", command=lambda: self._run("win")).pack(side=tk.LEFT, padx=6)
        ttk.Button(bf, text="Тест ESP32", command=lambda: self._run("esp32")).pack(side=tk.LEFT, padx=6)

        ttk.Label(
            self,
            text="Отправляет тестовое изображение и проверяет целостность ответа (как main.py).",
            wraplength=480,
        ).grid(row=2, column=0, columnspan=2, sticky=tk.W, padx=4, pady=8)

    def _run(self, destination: str) -> None:
        try:
            self.session.require_client()
        except RuntimeError as e:
            messagebox.showwarning("Диагностика", str(e))
            return
        try:
            size_kb = int(self.size_kb_var.get().strip())
        except ValueError:
            size_kb = 4
        if size_kb < 1:
            size_kb = 4

        def work():
            c = self.session.require_client()
            img = generate_test_image(size_kb=size_kb)
            received = c.test_component(destination, img)
            if received is None:
                return destination, False, "Нет ответа"
            valid, msg = verify_image_integrity(img, received)
            return destination, valid, msg

        self.session.log(f"[diag] Тест {destination}…")

        def ok(result):
            dest, valid, msg = result
            self.session.log(f"[diag] {dest}: {msg}")
            if valid:
                self.session.status(f"Диагностика: {dest} OK ({msg}).")
            else:
                messagebox.showwarning("Диагностика", f"{dest}: {msg}")

        def err(e):
            self.session.log(f"[diag] Ошибка: {e}")
            messagebox.showerror("Диагностика", str(e))

        run_in_thread(self.root, work, ok, err)

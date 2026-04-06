"""Вкладка: отправка фона на LCD (RGB565, чанки)."""

from __future__ import annotations

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from gui.threading_utils import run_in_thread
from image_utils import image_to_rgb565_be, load_bg_image


class DisplayTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root

        self.path_var = tk.StringVar(value="bg.jpg")
        r = 0
        ttk.Label(self, text="Файл изображения:").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(self, textvariable=self.path_var, width=48).grid(row=r, column=1, sticky=tk.EW, padx=4, pady=2)
        ttk.Button(self, text="Обзор…", command=self._browse).grid(row=r, column=2, padx=4, pady=2)
        r += 1

        ttk.Label(self, text="Строк в чанке (chunk_rows):").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.chunk_rows_var = tk.StringVar(value="20")
        ttk.Entry(self, textvariable=self.chunk_rows_var, width=8).grid(row=r, column=1, sticky=tk.W, padx=4, pady=2)
        r += 1

        ttk.Button(self, text="Отправить кадр на ESP32", command=self._send).grid(
            row=r, column=0, columnspan=3, pady=12
        )
        self.columnconfigure(1, weight=1)

        ttk.Label(
            self,
            text="Изображение приводится к 480×320 (letterbox), как в show_bg.py.",
            wraplength=520,
        ).grid(row=r + 1, column=0, columnspan=3, sticky=tk.W, padx=4, pady=8)

    def _browse(self) -> None:
        p = filedialog.askopenfilename(
            title="Изображение для дисплея",
            filetypes=[
                ("Изображения", "*.jpg *.jpeg *.png *.bmp"),
                ("Все файлы", "*.*"),
            ],
        )
        if p:
            self.path_var.set(p)

    def _send(self) -> None:
        try:
            self.session.require_client()
        except RuntimeError as e:
            messagebox.showwarning("Дисплей", str(e))
            return

        path = self.path_var.get().strip()
        if not path:
            messagebox.showwarning("Дисплей", "Укажите путь к файлу.")
            return
        try:
            chunk_rows = int(self.chunk_rows_var.get().strip())
        except ValueError:
            chunk_rows = 20
        if chunk_rows < 1:
            chunk_rows = 20

        dest = self.session.destination

        def work():
            img = load_bg_image(path)
            w, h = img.size
            frame_bytes = image_to_rgb565_be(img)
            cl = self.session.require_client()
            return cl.send_lcd_frame_rgb565(
                destination=dest,
                frame_bytes=frame_bytes,
                width=w,
                height=h,
                chunk_rows=chunk_rows,
            )

        self.session.log("[display] Отправка кадра…")

        def ok(resp):
            if resp is None:
                self.session.log("[display] Нет ответа (таймаут).")
                self.session.status("Дисплей: нет финального ответа (таймаут).")
            else:
                self.session.log(f"[display] Ответ: type={resp.get('type')}")
                self.session.status("Дисплей: кадр отправлен.")

        def err(e):
            self.session.log(f"[display] Ошибка: {e}")
            messagebox.showerror("Дисплей", str(e))

        run_in_thread(self.root, work, ok, err)

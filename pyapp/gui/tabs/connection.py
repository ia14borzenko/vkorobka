"""Вкладка: подключение к win-x64."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk


class ConnectionTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root

        r = 0
        ttk.Label(self, text="Хост:").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.host_var = tk.StringVar(value=session.host)
        ttk.Entry(self, textvariable=self.host_var, width=24).grid(row=r, column=1, sticky=tk.EW, padx=4, pady=2)
        r += 1

        ttk.Label(self, text="Порт UDP:").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.port_var = tk.StringVar(value=str(session.port))
        ttk.Entry(self, textvariable=self.port_var, width=8).grid(row=r, column=1, sticky=tk.W, padx=4, pady=2)
        r += 1

        ttk.Label(self, text="Таймаут (с):").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.timeout_var = tk.StringVar(value=str(session.timeout))
        ttk.Entry(self, textvariable=self.timeout_var, width=8).grid(row=r, column=1, sticky=tk.W, padx=4, pady=2)
        r += 1

        ttk.Label(self, text="Назначение команд:").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.dest_var = tk.StringVar(value=session.destination)
        ttk.Entry(self, textvariable=self.dest_var, width=12).grid(row=r, column=1, sticky=tk.W, padx=4, pady=2)
        r += 1

        self.verbose_var = tk.BooleanVar(value=session.verbose_udp)
        ttk.Checkbutton(self, text="Подробный лог UDP (verbose_udp)", variable=self.verbose_var).grid(
            row=r, column=0, columnspan=2, sticky=tk.W, padx=4, pady=4
        )
        r += 1

        bf = ttk.Frame(self)
        bf.grid(row=r, column=0, columnspan=2, pady=8)
        ttk.Button(bf, text="Подключить", command=self._on_connect).pack(side=tk.LEFT, padx=4)
        ttk.Button(bf, text="Отключить", command=self._on_disconnect).pack(side=tk.LEFT, padx=4)

        self.columnconfigure(1, weight=1)

        ttk.Label(
            self,
            text="Перед работой с дисплеем, микрофоном и динамиком нажмите «Подключить».",
            wraplength=480,
        ).grid(row=r + 1, column=0, columnspan=2, sticky=tk.W, padx=4, pady=12)

    def _read_network_fields(self) -> None:
        self.session.host = self.host_var.get().strip() or "127.0.0.1"
        try:
            self.session.port = int(self.port_var.get().strip())
        except ValueError:
            self.session.port = 1236
        try:
            self.session.timeout = float(self.timeout_var.get().strip())
        except ValueError:
            self.session.timeout = 30.0
        self.session.destination = self.dest_var.get().strip() or "esp32"
        self.session.verbose_udp = self.verbose_var.get()

    def _on_connect(self) -> None:
        self._read_network_fields()
        try:
            self.session.connect()
        except Exception as e:
            from tkinter import messagebox

            messagebox.showerror("Подключение", str(e))

    def _on_disconnect(self) -> None:
        self.session.disconnect()

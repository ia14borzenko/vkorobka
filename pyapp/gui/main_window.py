"""Главное окно: вкладки + журнал."""

from __future__ import annotations

import tkinter as tk
from tkinter import scrolledtext, ttk

from gui.session import AppSession
from gui.tabs import (
    ConnectionTab,
    DiagnosticTab,
    DisplayTab,
    SpeakerTab,
    TextingTab,
    VoiceTab,
)


def main() -> None:
    root = tk.Tk()
    root.title("VKOROBKA — управление ESP32-S3")
    root.minsize(800, 640)
    root.geometry("960x760")

    lf = ttk.LabelFrame(root, text="Журнал")
    log_widget = scrolledtext.ScrolledText(lf, height=11, wrap=tk.WORD)

    def log(msg: str) -> None:
        def append() -> None:
            log_widget.insert(tk.END, msg + "\n")
            log_widget.see(tk.END)

        root.after(0, append)

    session = AppSession(log)

    nb = ttk.Notebook(root)
    nb.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    nb.add(ConnectionTab(nb, session, root), text="Сеть")
    nb.add(DisplayTab(nb, session, root), text="Дисплей")
    nb.add(TextingTab(nb, session, root), text="Текстинг")
    nb.add(VoiceTab(nb, session, root), text="Микрофон")
    nb.add(SpeakerTab(nb, session, root), text="Динамик")
    nb.add(DiagnosticTab(nb, session, root), text="Диагностика")

    lf.pack(fill=tk.BOTH, expand=False, padx=4, pady=(0, 4), side=tk.BOTTOM)
    log_widget.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    def on_close() -> None:
        session.disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    log("VKOROBKA GUI: подключитесь на вкладке «Сеть», затем используйте остальные вкладки.")
    root.mainloop()

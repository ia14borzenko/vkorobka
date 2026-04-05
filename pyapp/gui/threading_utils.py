"""Запуск блокирующих операций в фоне с колбэком в GUI-поток."""

from __future__ import annotations

import threading
import traceback
from typing import Callable, Optional, TypeVar

T = TypeVar("T")


def run_in_thread(
    root,
    fn: Callable[[], T],
    on_success: Callable[[T], None],
    on_error: Optional[Callable[[BaseException], None]] = None,
) -> None:
    def worker() -> None:
        try:
            result = fn()
            root.after(0, lambda r=result: on_success(r))
        except BaseException as e:
            tb = traceback.format_exc()

            def fail() -> None:
                if on_error:
                    on_error(e)
                else:
                    from tkinter import messagebox

                    messagebox.showerror("Ошибка", f"{e}\n\n{tb}")

            root.after(0, fail)

    threading.Thread(target=worker, daemon=True).start()

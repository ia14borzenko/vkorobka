"""Вкладка: текстинг на ILI9486 (TextingManager)."""

from __future__ import annotations

import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, scrolledtext, ttk

from font_utils import ALL_CHARS, create_char_map, generate_char_images
from gui.threading_utils import run_in_thread
from texting import TextingManager


class TextingTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root

        r = 0
        ttk.Label(self, text="Шрифт (TTF/OTF):").grid(row=r, column=0, sticky=tk.NW, padx=4, pady=2)
        self.font_var = tk.StringVar(value="fonts/font.ttf")
        ttk.Entry(self, textvariable=self.font_var, width=40).grid(row=r, column=1, sticky=tk.EW, padx=4, pady=2)
        ttk.Button(self, text="Обзор…", command=self._browse_font).grid(row=r, column=2, padx=4, pady=2)
        r += 1

        self.regen_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(self, text="Полная регенерация символов и char_map.json перед действием", variable=self.regen_var).grid(
            row=r, column=0, columnspan=3, sticky=tk.W, padx=4, pady=2
        )
        r += 1

        pf = ttk.LabelFrame(self, text="Поле")
        pf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        for i, (lab, default) in enumerate(
            [
                ("field_x", "52"),
                ("field_y", "160"),
                ("field_width", "380"),
                ("field_height", "120"),
            ]
        ):
            ttk.Label(pf, text=lab).grid(row=i // 2, column=(i % 2) * 2, sticky=tk.W, padx=4, pady=2)
            v = tk.StringVar(value=default)
            setattr(self, f"_{lab}_var", v)
            ttk.Entry(pf, textvariable=v, width=8).grid(row=i // 2, column=(i % 2) * 2 + 1, padx=4, pady=2)
        r += 1

        tf = ttk.LabelFrame(self, text="Символы и скорость")
        tf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        ttk.Label(tf, text="char_height").grid(row=0, column=0, sticky=tk.W, padx=4, pady=2)
        self.char_h_var = tk.StringVar(value="14")
        ttk.Entry(tf, textvariable=self.char_h_var, width=6).grid(row=0, column=1, padx=4, pady=2)
        ttk.Label(tf, text="line_spacing").grid(row=0, column=2, sticky=tk.W, padx=4, pady=2)
        self.line_sp_var = tk.StringVar(value="2")
        ttk.Entry(tf, textvariable=self.line_sp_var, width=6).grid(row=0, column=3, padx=4, pady=2)
        ttk.Label(tf, text="typing_speed_ms").grid(row=1, column=0, sticky=tk.W, padx=4, pady=2)
        self.speed_var = tk.StringVar(value="50")
        ttk.Entry(tf, textvariable=self.speed_var, width=6).grid(row=1, column=1, padx=4, pady=2)
        r += 1

        ttk.Label(self, text="Текст для печати:").grid(row=r, column=0, sticky=tk.NW, padx=4, pady=2)
        self.text_w = scrolledtext.ScrolledText(self, width=60, height=6, wrap=tk.WORD)
        self.text_w.grid(row=r, column=1, columnspan=2, sticky=tk.EW, padx=4, pady=2)
        r += 1

        bf = ttk.Frame(self)
        bf.grid(row=r, column=0, columnspan=3, pady=8)
        ttk.Button(bf, text="Очистить поле", command=self._clear).pack(side=tk.LEFT, padx=4)
        ttk.Button(bf, text="Печать текста", command=self._add_text).pack(side=tk.LEFT, padx=4)

        self.columnconfigure(1, weight=1)

    def _browse_font(self) -> None:
        p = filedialog.askopenfilename(
            title="Шрифт",
            filetypes=[("Шрифты", "*.ttf *.otf"), ("Все файлы", "*.*")],
        )
        if p:
            self.font_var.set(p)

    def _int(self, var: tk.StringVar, default: int) -> int:
        try:
            return int(var.get().strip())
        except ValueError:
            return default

    def _maybe_regen_font(self, font_path: Path, char_height: int) -> bool:
        if not self.regen_var.get():
            return True
        chars_dir = font_path.parent / "chars"
        char_map_json = font_path.parent / "char_map.json"
        try:
            generate_char_images(
                font_path=str(font_path),
                output_dir=str(chars_dir),
                char_height=char_height,
                chars_to_generate=ALL_CHARS,
            )
            create_char_map(str(chars_dir), str(char_map_json))
            self.session.log("[texting] Регенерация символов завершена")
            return True
        except Exception as e:
            messagebox.showerror("Шрифт", str(e))
            return False

    def _build_manager(self) -> TextingManager | None:
        try:
            self.session.require_client()
        except RuntimeError as e:
            messagebox.showwarning("Текстинг", str(e))
            return None

        font_path = Path(self.font_var.get().strip())
        if not font_path.is_file():
            messagebox.showerror("Текстинг", f"Шрифт не найден: {font_path}")
            return None

        ch = self._int(self.char_h_var, 14)
        if not self._maybe_regen_font(font_path, ch):
            return None

        fonts_dir = font_path.parent
        chars_dir = fonts_dir / "chars"
        char_map_json = fonts_dir / "char_map.json"

        try:
            return TextingManager(
                field_x=self._int(self._field_x_var, 52),
                field_y=self._int(self._field_y_var, 160),
                field_width=self._int(self._field_width_var, 380),
                field_height=self._int(self._field_height_var, 120),
                char_height=ch,
                line_spacing=self._int(self.line_sp_var, 2),
                typing_speed_ms=self._int(self.speed_var, 50),
                font_path=str(font_path),
                chars_dir=str(chars_dir),
                char_map_json=str(char_map_json),
                client=self.session.require_client(),
                destination=self.session.destination,
                command_timeout_s=min(4.0, self.session.timeout),
            )
        except Exception as e:
            messagebox.showerror("Текстинг", str(e))
            return None

    def _clear(self) -> None:

        def work():
            m = self._build_manager()
            if not m:
                return None
            m.clear_field()
            return True

        def ok(_):
            self.session.log("[texting] Поле очищено")
            self.session.status("Текстинг: поле очищено.")

        def err(e):
            self.session.log(f"[texting] Ошибка: {e}")

        run_in_thread(self.root, work, ok, err)

    def _add_text(self) -> None:
        text = self.text_w.get("1.0", tk.END).rstrip("\n")
        if not text:
            messagebox.showwarning("Текстинг", "Введите текст.")
            return

        def work():
            m = self._build_manager()
            if not m:
                return None
            m.add_text(text)
            return True

        def ok(_):
            self.session.log("[texting] Текст отправлен")
            self.session.status("Текстинг: текст отправлен.")

        def err(e):
            self.session.log(f"[texting] Ошибка: {e}")

        run_in_thread(self.root, work, ok, err)

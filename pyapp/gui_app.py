#!/usr/bin/env python3
"""
Точка входа GUI vkorobka. Запуск из каталога pyapp:

  python gui_app.py
"""
from __future__ import annotations

import os
import sys

_ROOT = os.path.dirname(os.path.abspath(__file__))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from gui.main_window import main

if __name__ == "__main__":
    main()

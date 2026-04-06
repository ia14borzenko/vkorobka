"""Абстракция событий back-end для Smart Speaker."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional


@dataclass
class ResponsePayload:
    """Результат обработки запроса от back-end."""

    audio_path: Optional[Path]
    text: str
    display_asset: Optional[Path] = None
    display_x: int = 0
    display_y: int = 0
    is_animation: bool = False


@dataclass
class BackendEvents:
    """Колбэки событий, которые back-end отдает оркестратору."""

    on_wake_detected: Callable[[], None]
    on_stop_capture: Callable[[], None]
    on_response_ready: Callable[[ResponsePayload], None]
    on_error: Callable[[str], None]


class BackendAdapterBase:
    """Интерфейс адаптера источника событий back-end."""

    def set_events(self, events: BackendEvents) -> None:
        self._events = events

    def start(self) -> None:
        """Запустить адаптер (подписка на события)."""

    def stop(self) -> None:
        """Остановить адаптер."""

    def submit_query_audio(self, query_wav: Path) -> None:
        """Передать записанный запрос на обработку в back-end."""


class MockBackendAdapter(BackendAdapterBase):
    """
    Мок-адаптер для тестового режима.

    События может генерировать UI-код через simulate_* методы.
    """

    def __init__(self) -> None:
        self._events: Optional[BackendEvents] = None
        self._running = False

    def start(self) -> None:
        self._running = True

    def stop(self) -> None:
        self._running = False

    def submit_query_audio(self, query_wav: Path) -> None:
        # В мок-режиме обработчик приходит по команде пользователя.
        _ = query_wav

    def simulate_wake_detected(self) -> None:
        if self._running and self._events:
            self._events.on_wake_detected()

    def simulate_stop_capture(self) -> None:
        if self._running and self._events:
            self._events.on_stop_capture()

    def simulate_response_ready(self, payload: ResponsePayload) -> None:
        if self._running and self._events:
            self._events.on_response_ready(payload)

    def simulate_error(self, message: str) -> None:
        if self._running and self._events:
            self._events.on_error(message)

"""Общая сессия: параметры сети и единственный VkorobkaClient."""

from __future__ import annotations

from typing import Callable, Optional

from vkorobka_client import VkorobkaClient


class AppSession:
    def __init__(self, log: Callable[[str], None]):
        self._log = log
        self.host: str = "127.0.0.1"
        self.port: int = 1236
        self.timeout: float = 30.0
        self.verbose_udp: bool = False
        self.destination: str = "esp32"
        self._client: Optional[VkorobkaClient] = None

    def log(self, msg: str) -> None:
        self._log(msg)

    @property
    def client(self) -> Optional[VkorobkaClient]:
        return self._client

    def is_connected(self) -> bool:
        return self._client is not None

    def connect(self) -> None:
        if self._client:
            try:
                self._client.close()
            except Exception:
                pass
            self._client = None
        self._client = VkorobkaClient(
            server_host=self.host,
            server_port=self.port,
            timeout=self.timeout,
            verbose_udp=self.verbose_udp,
        )
        self.log(f"[session] Подключено к {self.host}:{self.port}, timeout={self.timeout}s")

    def disconnect(self) -> None:
        if self._client:
            try:
                self._client.close()
            except Exception:
                pass
            self._client = None
            self.log("[session] Отключено")

    def require_client(self) -> VkorobkaClient:
        if not self._client:
            raise RuntimeError("Сначала подключитесь к win-x64 (вкладка «Сеть»).")
        return self._client

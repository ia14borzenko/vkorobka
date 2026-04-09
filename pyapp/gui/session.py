"""Общая сессия: параметры сети и единственный VkorobkaClient."""

from __future__ import annotations

from typing import Callable, Optional

from vkorobka_client import VkorobkaClient


class AppSession:
    def __init__(self, log: Callable[[str], None]):
        self._log = log
        self._status: Optional[Callable[[str], None]] = None
        self.host: str = "127.0.0.1"
        self.port: int = 1236
        self.timeout: float = 5.0
        self.verbose_udp: bool = False
        self.destination: str = "esp32"
        self._client: Optional[VkorobkaClient] = None
        # Общие настройки микрофона (для всех вкладок/сценариев).
        self.mic_rate_hz: int = 48000
        self.mic_bits: int = 24
        self.mic_gain_db: float = 3.0
        self.mic_chunk_samples: int = 512
        self.mic_mute: bool = False
        self.mic_clip: bool = True
        self.mic_record_gain_db: float = 0.0
        # Общие настройки динамика (для всех вкладок/сценариев).
        self.speaker_chunk_samples: int = 512
        self.speaker_pace_factor: float = 0.97
        self.speaker_no_pace: bool = False
        self.speaker_command_timeout_s: float = 4.0
        self.speaker_dyn_rate_hz: int = VkorobkaClient.DYN_PCM_SAMPLE_RATE_HZ
        self.speaker_dyn_gain_db: float = 0.0
        self.speaker_skip_dyn_set: bool = False
        self.speaker_dyn_mute: bool = False
        self.speaker_dyn_clip: bool = True
        self.speaker_flow_control: bool = False
        self.speaker_adaptive_pace: bool = False
        self.speaker_max_ack_wait_s: float = 0.005
        self.speaker_flow_window: int = 8
        # Настройки адаптивного порога тишины (источник: вкладка "Умная колонка").
        self.smart_silence_threshold: float = 6000.0
        self.smart_silence_adaptive: bool = True
        self.smart_silence_noise_alpha: float = 0.04
        self.smart_silence_multiplier: float = 2.2

    def log(self, msg: str) -> None:
        self._log(msg)

    def set_status_callback(self, cb: Callable[[str], None]) -> None:
        self._status = cb

    def status(self, msg: str) -> None:
        if self._status:
            self._status(msg)
        self._log(f"[status] {msg}")

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
            command_logger=self._on_client_command,
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

    def _on_client_command(self, destination: str, command: str, test_id: str) -> None:
        if destination != "esp32":
            return
        self.log(f"[esp32-cmd] test_id={test_id} cmd={command}")

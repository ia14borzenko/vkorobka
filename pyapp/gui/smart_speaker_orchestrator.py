"""Оркестратор сценария Smart Speaker."""

from __future__ import annotations

import tempfile
import threading
import time
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import numpy as np

from gui.backend_contract import BackendAdapterBase, BackendEvents, ResponsePayload
from texting import TextingManager
from vkorobka_client import VkorobkaClient
from voice_cli import STREAM_ID_MIC, parse_mic_stream_payload, save_audio_flac_and_wav


@dataclass
class TextingConfig:
    font_path: str = "fonts/font.ttf"
    field_x: int = 52
    field_y: int = 160
    field_width: int = 380
    field_height: int = 120
    char_height: int = 14
    line_spacing: int = 2
    manual_chars_per_sec: float = 20.0
    auto_speed: bool = True
    min_auto_cps: float = 8.0
    max_auto_cps: float = 30.0


@dataclass
class VoiceConfig:
    rate_hz: int = 48000
    bits: int = 24
    gain_db: float = 3.0
    chunk_samples: int = 512
    mute: bool = False
    clip: bool = True
    record_gain_db: float = 0.0
    stop_mode: str = "silence"  # silence|external
    silence_threshold: float = 6000.0
    silence_seconds: float = 3.0


class SmartSpeakerOrchestrator:
    STATE_IDLE = "idleWaitWake"
    STATE_CAPTURE = "captureQuery"
    STATE_WAIT_BACKEND = "waitBackendResult"
    STATE_RENDER = "renderResponse"
    STATE_STOPPED = "stopped"

    def __init__(
        self,
        *,
        root,
        client_getter: Callable[[], VkorobkaClient],
        destination_getter: Callable[[], str],
        log: Callable[[str], None],
        backend_adapter: BackendAdapterBase,
        texting_cfg_getter: Callable[[], TextingConfig],
        voice_cfg_getter: Callable[[], VoiceConfig],
        state_changed: Callable[[str], None],
    ) -> None:
        self.root = root
        self.client_getter = client_getter
        self.destination_getter = destination_getter
        self.log = log
        self.backend = backend_adapter
        self.texting_cfg_getter = texting_cfg_getter
        self.voice_cfg_getter = voice_cfg_getter
        self.state_changed = state_changed

        self._lock = threading.RLock()
        self._running = False
        self._state = self.STATE_STOPPED
        self._chunks: List[Tuple[Any, int]] = []
        self._sample_rate = 48000
        self._bits = 24
        self._last_above_threshold_ts = 0.0
        self._stop_capture_requested = False
        self._voice_stream_started = False
        self._render_in_progress = False

    def start(self) -> None:
        with self._lock:
            if self._running:
                return
            self._running = True
            self._set_state(self.STATE_IDLE)
            self._chunks.clear()
            self._stop_capture_requested = False

        self.backend.set_events(
            BackendEvents(
                on_wake_detected=self._on_wake_detected,
                on_stop_capture=self._on_external_stop_capture,
                on_response_ready=self._on_response_ready,
                on_error=self._on_backend_error,
            )
        )
        self.backend.start()
        self._start_wake_listening_stream()
        self.log("[smart] Сценарий запущен, ожидание wake-up фразы")

    def stop(self) -> None:
        with self._lock:
            if not self._running:
                return
            self._running = False
        self.backend.stop()
        self._safe_voice_off()
        self._set_state(self.STATE_STOPPED)
        self.log("[smart] Сценарий остановлен")

    def _set_state(self, state: str) -> None:
        with self._lock:
            self._state = state
        self.root.after(0, lambda s=state: self.state_changed(s))

    def _on_backend_error(self, message: str) -> None:
        self.log(f"[smart] backend error: {message}")

    def _on_wake_detected(self) -> None:
        with self._lock:
            if not self._running or self._state != self.STATE_IDLE:
                return
        self.log("[smart] Wake-up обнаружен, начинаю запись запроса")
        self._start_capture()

    def _on_external_stop_capture(self) -> None:
        with self._lock:
            if not self._running or self._state != self.STATE_CAPTURE:
                return
        self.log("[smart] Внешняя команда остановки записи")
        self._stop_capture_async()

    def _on_response_ready(self, payload: ResponsePayload) -> None:
        with self._lock:
            if not self._running:
                return
            if self._render_in_progress:
                self.log("[smart] response_ready игнор: рендер уже выполняется")
                return
            if self._state != self.STATE_WAIT_BACKEND:
                self.log(f"[smart] response_ready в состоянии {self._state}, игнор")
                return
            self._render_in_progress = True
            self._set_state(self.STATE_RENDER)
        self._pause_wake_stream_for_render()
        self.log("[smart] Получен ответ backend, запускаю вывод")
        threading.Thread(target=self._render_response, args=(payload,), daemon=True).start()

    def _start_capture(self) -> None:
        with self._lock:
            self._chunks.clear()
            self._last_above_threshold_ts = time.time()
            self._stop_capture_requested = False
        self._set_state(self.STATE_CAPTURE)
        self.log("[smart] Запись запроса началась")

    def _start_wake_listening_stream(self) -> None:
        vc = self.voice_cfg_getter()
        client = self.client_getter()
        client.register_stream_handler(STREAM_ID_MIC, self._mic_handler)
        if not client.send_voice_set(
            rate_hz=vc.rate_hz,
            bits=vc.bits,
            gain_db=vc.gain_db,
            chunk_samples=vc.chunk_samples,
            mute=vc.mute,
            clip=vc.clip,
            command_timeout=4.0,
        ):
            raise RuntimeError("voice.set отклонён при запуске wake-listening")
        tid = client.send_command(self.destination_getter(), "voice.on")
        resp = client.wait_for_response(tid, timeout=4.0)
        if not resp:
            raise RuntimeError("Нет ответа на voice.on при запуске wake-listening")
        self._voice_stream_started = True
        self.log("[smart] Непрерывный поток микрофона в backend запущен")

    def _mic_handler(self, message: Dict[str, Any]) -> None:
        payload = message.get("payload") or ""
        if not payload:
            return
        try:
            rate, block, bits = parse_mic_stream_payload(payload)
            arr = np.asarray(block, dtype=np.int32 if bits == 24 else np.int16)
            with self._lock:
                state_now = self._state
            if state_now == self.STATE_IDLE:
                # Непрерывная передача wake-listening аудио в backend.
                self.backend.submit_wake_audio_chunk(rate, bits, arr.copy())
            with self._lock:
                self._sample_rate = rate
                self._bits = bits
                if self._state != self.STATE_CAPTURE:
                    return
            level = float(np.mean(np.abs(arr))) if arr.size else 0.0
            now = time.time()
            vc = self.voice_cfg_getter()
            with self._lock:
                self._chunks.append((arr, bits))
                if vc.stop_mode == "silence":
                    if level >= vc.silence_threshold:
                        self._last_above_threshold_ts = now
                    elif (
                        not self._stop_capture_requested
                        and (now - self._last_above_threshold_ts) >= vc.silence_seconds
                    ):
                        self._stop_capture_requested = True
                        self.log("[smart] Тишина обнаружена, останавливаю запись")
                        self._stop_capture_async()
        except Exception as e:
            self.log(f"[smart] Ошибка парсинга микрофона: {e}")

    def _stop_capture_async(self) -> None:
        threading.Thread(target=self._stop_capture, daemon=True).start()

    def _stop_capture(self) -> None:
        with self._lock:
            if self._state != self.STATE_CAPTURE:
                return
        with self._lock:
            sr = self._sample_rate
            bits = self._bits
            chunks = list(self._chunks)
        if not chunks:
            merged = np.array([], dtype=np.int16 if bits == 16 else np.int32)
        else:
            merged = np.concatenate([np.asarray(c[0]) for c in chunks])

        query_wav = self._persist_query_audio(merged, sr, bits)
        self._set_state(self.STATE_WAIT_BACKEND)
        self._show_waiting_backend_text()
        self.log(f"[smart] Запрос записан: {query_wav}")
        self.backend.submit_query_audio(query_wav)

    def _persist_query_audio(self, merged: np.ndarray, sample_rate: int, bits: int) -> Path:
        tmp_dir = Path(tempfile.gettempdir()) / "vkorobka_smart"
        tmp_dir.mkdir(parents=True, exist_ok=True)
        ts = int(time.time() * 1000)
        flac_path = tmp_dir / f"query_{ts}.flac"
        wav_path = tmp_dir / f"query_{ts}.wav"
        if merged.size == 0:
            # Пустой WAV-заглушка, чтобы контракт не ломался.
            with wave.open(str(wav_path), "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(max(8000, int(sample_rate)))
                wf.writeframes(b"")
            return wav_path
        save_audio_flac_and_wav(flac_path, wav_path, merged, int(sample_rate), int(bits))
        return wav_path

    def _safe_voice_off(self) -> None:
        try:
            client = self.client_getter()
            if self._voice_stream_started:
                tid = client.send_command(self.destination_getter(), "voice.off")
                client.wait_for_response(tid, timeout=4.0)
            client.register_stream_handler(STREAM_ID_MIC, lambda _m: None)
            self._voice_stream_started = False
        except Exception:
            pass

    def _pause_wake_stream_for_render(self) -> None:
        # На некоторых прошивках dyn.* конфликтует с активным voice.on.
        self._safe_voice_off()
        self.log("[smart] Wake-stream временно приостановлен на время вывода ответа")

    def _render_response(self, payload: ResponsePayload) -> None:
        err: Optional[Exception] = None
        try:
            # По новому UX не рисуем картинки в режиме ожидания/ответа.
            if payload.audio_path and payload.text:
                self._play_audio_and_text_interleaved(payload)
            else:
                self._play_audio_if_present(payload)
                self._send_text_if_present(payload)
            self.log("[smart] Вывод ответа завершен")
        except Exception as e:
            err = e
            self.log(f"[smart] Ошибка вывода ответа: {e}")
        finally:
            if err is not None:
                self.log("[smart] Возврат в idle после ошибки")
            with self._lock:
                still_running = self._running
            if still_running:
                try:
                    self._start_wake_listening_stream()
                except Exception as e:
                    self.log(f"[smart] Ошибка перезапуска wake-stream: {e}")
                self._set_state(self.STATE_IDLE)
            with self._lock:
                self._render_in_progress = False

    def _show_waiting_backend_text(self) -> None:
        try:
            cfg = self.texting_cfg_getter()
            manager = self._build_texting_manager(cfg, typing_speed_ms=35)
            manager.clear_field()
            manager.add_text("ожидаем ответа ...")
            self.log("[smart] На дисплей выведено: ожидаем ответа ...")
        except Exception as e:
            self.log(f"[smart] Не удалось вывести текст ожидания: {e}")

    def _play_audio_and_text_interleaved(self, payload: ResponsePayload) -> None:
        path = Path(payload.audio_path) if payload.audio_path else None
        if not path or not path.is_file():
            self.log("[smart] interleave: аудио не найдено, отправляю только текст")
            self._send_text_if_present(payload)
            return

        text = payload.text or ""
        if not text:
            self._play_audio_if_present(payload)
            return

        cfg = self.texting_cfg_getter()
        manager = self._build_texting_manager(cfg, typing_speed_ms=1)
        manager.clear_field()

        duration_s = self._audio_duration_sec(path) or 0.0
        chars = list(text)
        n_chars = len(chars)
        state = {
            "idx": 0,
            "chunk_interval": None,
            "logged": False,
        }

        def on_chunk_sent(seq: int, total_chunks: int, avg_chunk_send_s: float) -> None:
            if state["idx"] >= n_chars:
                return
            if state["chunk_interval"] is None:
                if duration_s > 0.01:
                    symbol_period_s = duration_s / float(n_chars)
                    interval = int(round(symbol_period_s / max(1e-6, avg_chunk_send_s)))
                else:
                    interval = int(round(total_chunks / float(max(1, n_chars))))
                state["chunk_interval"] = max(1, interval)
                if not state["logged"]:
                    self.log(
                        "[smart] interleave: "
                        f"chars={n_chars}, duration={duration_s:.3f}s, "
                        f"avg_chunk_send={avg_chunk_send_s*1000.0:.3f}ms, "
                        f"chunk_interval={state['chunk_interval']}"
                    )
                    state["logged"] = True

            if seq % int(state["chunk_interval"]) == 0 and state["idx"] < n_chars:
                manager.add_text(chars[state["idx"]])
                state["idx"] += 1

        client = self.client_getter()
        ok = client.play_audio_file_to_esp32_dyn(
            str(path),
            on_chunk_sent=on_chunk_sent,
        )
        if not ok:
            self.log("[smart] Предупреждение: interleave audio playback завершился с ошибкой")

        # Досылаем хвост текста, если аудио кончилось раньше, чем символы.
        while state["idx"] < n_chars:
            manager.add_text(chars[state["idx"]])
            state["idx"] += 1

    def _play_audio_if_present(self, payload: ResponsePayload) -> None:
        if not payload.audio_path:
            return
        path = Path(payload.audio_path)
        if not path.is_file():
            self.log(f"[smart] Аудио не найдено: {path}")
            return
        client = self.client_getter()
        ok = client.play_audio_file_to_esp32_dyn(str(path))
        if not ok:
            self.log("[smart] Предупреждение: audio playback завершился с ошибкой")

    def _send_text_if_present(self, payload: ResponsePayload) -> None:
        if not payload.text:
            return
        cfg = self.texting_cfg_getter()
        typing_speed_ms = self._resolve_typing_speed_ms(payload.text, payload.audio_path, cfg)
        manager = self._build_texting_manager(cfg, typing_speed_ms)
        manager.clear_field()
        manager.add_text(payload.text)
        self.log(f"[smart] Текст отправлен, speed={typing_speed_ms} ms/char")

    def _build_texting_manager(self, cfg: TextingConfig, typing_speed_ms: int) -> TextingManager:
        font_path = Path(cfg.font_path)
        fonts_dir = font_path.parent
        return TextingManager(
            field_x=cfg.field_x,
            field_y=cfg.field_y,
            field_width=cfg.field_width,
            field_height=cfg.field_height,
            char_height=cfg.char_height,
            line_spacing=cfg.line_spacing,
            typing_speed_ms=typing_speed_ms,
            font_path=str(font_path),
            chars_dir=str(fonts_dir / "chars"),
            char_map_json=str(fonts_dir / "char_map.json"),
            client=self.client_getter(),
            destination=self.destination_getter(),
            command_timeout_s=4.0,
        )

    def _resolve_typing_speed_ms(
        self,
        text: str,
        audio_path: Optional[Path],
        cfg: TextingConfig,
    ) -> int:
        cps = float(cfg.manual_chars_per_sec)
        if cfg.auto_speed:
            duration = self._audio_duration_sec(audio_path) if audio_path else None
            if duration and duration > 0.05 and len(text) > 0:
                cps = len(text) / duration
                cps = max(cfg.min_auto_cps, min(cfg.max_auto_cps, cps))
            else:
                cps = max(cfg.min_auto_cps, min(cfg.max_auto_cps, cps))
        cps = max(1.0, cps)
        return max(1, int(round(1000.0 / cps)))

    def _audio_duration_sec(self, audio_path: Path) -> Optional[float]:
        p = Path(audio_path)
        if not p.is_file():
            return None
        if p.suffix.lower() == ".wav":
            try:
                with wave.open(str(p), "rb") as wf:
                    nframes = wf.getnframes()
                    fr = wf.getframerate()
                    if fr > 0:
                        return float(nframes) / float(fr)
            except Exception:
                return None
        try:
            import soundfile as sf

            info = sf.info(str(p))
            if info.samplerate > 0:
                return float(info.frames) / float(info.samplerate)
        except Exception:
            return None
        return None

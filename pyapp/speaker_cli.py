#!/usr/bin/env python3
"""
Воспроизведение аудиофайла на динамике ESP32-S3 (MAX98357A) через pyapp → win-x64 → TCP → ESP32.

Передаются несжатые отсчёты: моно PCM 24-bit little-endian в payload, 48 kHz, тот же формат,
что uplink с микрофона (stream_id=2), для динамика используется stream_id=3.

Последовательность: команда dyn.on → серия MSG_TYPE_STREAM (чанки до 512 сэмплов) → dyn.off.

Запуск:
  python speaker_cli.py --host 127.0.0.1 --port 1236 path/to/audio.wav
  python speaker_cli.py music.flac --no-pace

Требования: numpy, soundfile; для файлов не 48 kHz — scipy (ресэмплинг).
Перед запуском: win-x64 и прошивка ESP32 с поддержкой dyn.on / dyn.off и I2S на усилитель.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from vkorobka_client import VkorobkaClient


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Вывод WAV/FLAC на динамик ESP32 (48 kHz, PCM24 по протоколу vkorobka)"
    )
    parser.add_argument("audio_file", type=Path, help="WAV, FLAC и др. (через soundfile)")
    parser.add_argument("--host", default="127.0.0.1", help="UDP хост win-x64")
    parser.add_argument("--port", type=int, default=1236, help="UDP порт win-x64")
    parser.add_argument(
        "--chunk-samples",
        type=int,
        default=512,
        help="Сэмплов моно на один STREAM-пакет (1..512)",
    )
    parser.add_argument(
        "--no-pace",
        action="store_true",
        help="Не ждать между чанками (быстрее, риск переполнения очереди на ESP32)",
    )
    parser.add_argument(
        "--pace-factor",
        type=float,
        default=0.97,
        help="Доля реального времени между чанками при pace (0..1)",
    )
    parser.add_argument("--timeout", type=float, default=30.0, help="Таймаут UDP recv")
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=15.0,
        help="Таймаут ожидания DYN_ON_OK / DYN_OFF_OK",
    )
    args = parser.parse_args()

    if not 1 <= args.chunk_samples <= 512:
        print("--chunk-samples must be 1..512", file=sys.stderr)
        return 2

    client = VkorobkaClient(
        server_host=args.host,
        server_port=args.port,
        timeout=args.timeout,
        verbose_udp=False,
    )
    try:
        ok = client.play_audio_file_to_esp32_dyn(
            str(args.audio_file),
            chunk_samples=args.chunk_samples,
            pace=not args.no_pace,
            pace_factor=args.pace_factor,
            command_timeout=args.command_timeout,
        )
        return 0 if ok else 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

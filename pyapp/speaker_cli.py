#!/usr/bin/env python3
"""
Воспроизведение аудиофайла на динамике ESP32-S3 (MAX98357A) через pyapp → win-x64 → TCP → ESP32.

Передаются несжатые отсчёты: моно PCM 16-bit little-endian, 22050 Hz, stream_id=3
(меньше нагрузка на сеть, чем 48 kHz / 24-bit).

Последовательность: команда dyn.on → серия MSG_TYPE_STREAM (чанки до 512 сэмплов) → dyn.off.

Запуск:
  python speaker_cli.py --host 127.0.0.1 --port 1236 path/to/audio.wav
  python speaker_cli.py music.flac --no-pace

Требования: numpy, soundfile; scipy — ресэмплинг в 22050 Hz при другой частоте файла.
Перед запуском: win-x64 и прошивка ESP32 с поддержкой dyn.on / dyn.off и I2S на усилитель.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from vkorobka_client import VkorobkaClient


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Отправка аудиофайла на динамик ESP32 (dyn.on -> PCM stream -> dyn.off).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Примеры:\n"
            "  python speaker_cli.py path/to/track.wav\n"
            "  python speaker_cli.py music.flac --dyn-rate 32000 --dyn-gain-db 3\n"
            "  python speaker_cli.py track.wav --no-pace --chunk-samples 256\n"
        ),
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
        default=4.0,
        help="Таймаут ожидания DYN_ON_OK / DYN_OFF_OK",
    )
    parser.add_argument(
        "--dyn-rate",
        type=int,
        default=VkorobkaClient.DYN_PCM_SAMPLE_RATE_HZ,
        help="Частота dyn.set / PCM (белый список как на ESP32)",
    )
    parser.add_argument(
        "--dyn-bits",
        type=int,
        default=16,
        choices=(16,),
        help="Разрядность (на устройстве поддерживается 16)",
    )
    parser.add_argument(
        "--dyn-gain-db",
        type=float,
        default=0.0,
        help="Усиление на ESP32 (dyn.set gain_db)",
    )
    parser.add_argument(
        "--skip-dyn-set",
        action="store_true",
        help="Не отправлять dyn.set (уже настроено на устройстве)",
    )
    args = parser.parse_args()

    if not 1 <= args.chunk_samples <= 512:
        print("--chunk-samples must be 1..512", file=sys.stderr)
        return 2
    if args.dyn_rate not in VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ:
        print(
            f"--dyn-rate must be one of {sorted(VkorobkaClient.DYN_ALLOWED_SAMPLE_RATES_HZ)}",
            file=sys.stderr,
        )
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
            dyn_rate_hz=args.dyn_rate,
            dyn_bits=args.dyn_bits,
            dyn_gain_db=args.dyn_gain_db,
            send_dyn_set=not args.skip_dyn_set,
        )
        return 0 if ok else 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

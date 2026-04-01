#!/usr/bin/env python3
"""
Мини-консоль для записи PCM с ESP32-S3 (INMP441) через win-x64 в файл FLAC.

Команды:
  voice.on   — отправить voice.on на ESP32, начать накопление сэмплов
  voice.off  — отправить voice.off, остановить приём, сохранить FLAC + TSV отсчётов
  quit       — выход (без сохранения, если не было voice.off)

Формат потока: 48 kHz, 24-bit LE (заголовок 8 байт + PCM).

Запуск:
  python voice_cli.py --host 127.0.0.1 --port 1236 -o capture.flac
  python voice_cli.py -o capture.flac --table-output my_samples.tsv
"""
from __future__ import annotations

import argparse
import array
import base64
import struct
import sys
import threading
from pathlib import Path
from typing import Any, Dict, List, Tuple, Union

from vkorobka_client import VkorobkaClient

STREAM_ID_MIC = 2


def _le24_bytes_to_int32(pcm: bytes, offset: int) -> int:
    b0 = pcm[offset]
    b1 = pcm[offset + 1]
    b2 = pcm[offset + 2]
    x = b0 | (b1 << 8) | (b2 << 16)
    if x & 0x800000:
        x -= 0x1000000
    return x


def parse_mic_stream_payload(payload_b64: str) -> Tuple[int, Union[array.array, "Any"], int]:
    """
    Возвращает (sample_rate, буфер сэмплов, bits_per_sample).
    24-bit: numpy int32; 16-bit: array int16 (совместимость).
    """
    import numpy as np

    raw = base64.b64decode(payload_b64)
    if len(raw) < 8:
        return 48000, np.array([], dtype=np.int32), 24
    sample_rate, n_samp, bits = struct.unpack_from("<IHH", raw, 0)
    pcm_bytes = raw[8:]
    if bits == 24:
        n = min(n_samp, len(pcm_bytes) // 3)
        out = np.empty(n, dtype=np.int32)
        for i in range(n):
            out[i] = _le24_bytes_to_int32(pcm_bytes, i * 3)
        return int(sample_rate), out, 24
    # legacy 16-bit
    n = min(n_samp, len(pcm_bytes) // 2)
    samples = array.array("h")
    samples.frombytes(pcm_bytes[: n * 2])
    return int(sample_rate), samples, 16


def save_audio_flac_or_wav(
    path: Path,
    samples: Union[array.array, Any],
    sample_rate: int,
    bits: int,
) -> Path:
    try:
        import numpy as np
        import soundfile as sf
    except ImportError as e:
        print(
            "[voice] Нет numpy/soundfile — pip install numpy soundfile",
            file=sys.stderr,
        )
        raise e

    path = path.expanduser()
    data = np.asarray(samples)
    if bits == 24:
        data = data.astype(np.int32, copy=False)
        subtype = "PCM_24"
    else:
        data = data.astype(np.int16, copy=False)
        subtype = "PCM_16"
    try:
        sf.write(str(path), data, sample_rate, format="FLAC", subtype=subtype)
        return path
    except Exception as e:
        wav_path = path.with_suffix(".wav")
        print(f"[voice] FLAC недоступен ({e}), пишем WAV: {wav_path}")
        sf.write(str(wav_path), data, sample_rate, subtype=subtype)
        return wav_path


def write_samples_table(path: Path, samples: Any, bits: int) -> None:
    import numpy as np

    path = path.expanduser()
    arr = np.asarray(samples).ravel()
    col = "sample_int24" if bits == 24 else "sample_int16"
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"index\t{col}\n")
        for idx in range(arr.shape[0]):
            f.write(f"{idx}\t{int(arr[idx])}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Запись голоса с ESP32 в FLAC + TSV")
    parser.add_argument("--host", default="127.0.0.1", help="UDP win-x64")
    parser.add_argument("--port", type=int, default=1236, help="UDP порт win-x64")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("voice_capture.flac"),
        help="Выходной .flac",
    )
    parser.add_argument(
        "--table-output",
        type=Path,
        default=None,
        help="TSV: index<TAB>sample (по умолчанию рядом с -o: stem_samples.tsv)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Таймаут ожидания ответов ESP (сек)",
    )
    args = parser.parse_args()

    chunks: List[Tuple[Union[array.array, Any], int]] = []
    sample_rate_ref: List[int] = [48000]
    bits_ref: List[int] = [24]
    recording = threading.Event()
    lock = threading.Lock()

    def on_mic_stream(message: Dict[str, Any]) -> None:
        if not recording.is_set():
            return
        pl = message.get("payload") or ""
        if not pl:
            return
        try:
            rate, block, bits = parse_mic_stream_payload(pl)
            with lock:
                sample_rate_ref[0] = rate
                bits_ref[0] = bits
                chunks.append((block, bits))
        except Exception as ex:
            print(f"[voice] Ошибка разбора STREAM: {ex}", file=sys.stderr)

    client = VkorobkaClient(
        server_host=args.host,
        server_port=args.port,
        timeout=args.timeout,
        verbose_udp=False,
    )
    client.register_stream_handler(STREAM_ID_MIC, on_mic_stream)

    table_path = args.table_output
    if table_path is None:
        table_path = args.output.with_name(args.output.stem + "_samples.tsv")

    print("Команды: voice.on | voice.off | quit")
    print(f"FLAC: {args.output.resolve()}")
    print(f"TSV:  {table_path.resolve()}")

    try:
        while True:
            try:
                line = input("voice> ").strip()
            except EOFError:
                print()
                break
            low = line.lower()
            if low in ("quit", "exit", "q"):
                break

            if low == "voice.on":
                with lock:
                    chunks.clear()
                recording.set()
                tid = client.send_command("esp32", "voice.on")
                resp = client.wait_for_response(tid, timeout=10.0)
                if resp:
                    print("[voice] ESP:", _response_payload_preview(resp))
                else:
                    print("[voice] Нет ответа на voice.on (проверьте TCP/UDP)")
                continue

            if low == "voice.off":
                recording.clear()
                tid = client.send_command("esp32", "voice.off")
                resp = client.wait_for_response(tid, timeout=10.0)
                if resp:
                    print("[voice] ESP:", _response_payload_preview(resp))
                else:
                    print("[voice] Нет ответа на voice.off")

                import numpy as np

                with lock:
                    sr = sample_rate_ref[0]
                    br = bits_ref[0]
                    if not chunks:
                        merged = np.array([], dtype=np.int32 if br == 24 else np.int16)
                    else:
                        first_bits = chunks[0][1]
                        if first_bits == 24:
                            merged = np.concatenate(
                                [np.asarray(c[0], dtype=np.int32) for c in chunks]
                            )
                        else:
                            merged = np.concatenate(
                                [
                                    np.asarray(c[0], dtype=np.int16)
                                    for c in chunks
                                ]
                            )
                    n = merged.shape[0]
                if n == 0:
                    print("[voice] Нет сэмплов — файлы не созданы")
                    continue
                out = save_audio_flac_or_wav(args.output, merged, sr, br)
                write_samples_table(table_path, merged, br)
                print(f"[voice] FLAC: {out.resolve()} ({n} samples, {sr} Hz, {br}-bit)")
                print(f"[voice] TSV:  {table_path.resolve()}")
                continue

            print("Неизвестная команда. Используйте: voice.on | voice.off | quit")
    finally:
        client.close()

    return 0


def _response_payload_preview(resp: Dict[str, Any]) -> str:
    import json

    pl = resp.get("payload")
    if not pl:
        return json.dumps(resp, ensure_ascii=False)[:200]
    try:
        raw = base64.b64decode(pl)
        return raw.decode("utf-8", errors="replace")
    except Exception:
        return str(pl)[:120]


if __name__ == "__main__":
    sys.exit(main())

"""Вкладка: запись микрофона (voice.set / voice.on / voice.off)."""

from __future__ import annotations

import base64
import json
import threading
import tkinter as tk
from pathlib import Path
from typing import Any, Dict, List, Tuple

from tkinter import messagebox, ttk

import numpy as np
try:
    import winsound
except Exception:  # pragma: no cover - non-Windows fallback
    winsound = None

from gui.threading_utils import run_in_thread
from voice_cli import (
    STREAM_ID_MIC,
    apply_record_gain_db,
    parse_mic_stream_payload,
    save_audio_flac_and_wav,
    write_samples_table,
)


class VoiceTab(ttk.Frame):
    def __init__(self, parent, session, root: tk.Tk):
        super().__init__(parent)
        self.session = session
        self.root = root

        self._chunks: List[Tuple[Any, int]] = []
        self._sample_rate_ref: List[int] = [48000]
        self._bits_ref: List[int] = [24]
        self._lock = threading.Lock()
        self._recording = threading.Event()
        self._last_wav_path: Path | None = None

        r = 0
        ttk.Label(self, text="Выход FLAC (-o):").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.out_var = tk.StringVar(value="voice_capture.flac")
        ttk.Entry(self, textvariable=self.out_var, width=44).grid(row=r, column=1, sticky=tk.EW, padx=4, pady=2)
        ttk.Button(self, text="Обзор…", command=self._browse_out).grid(row=r, column=2, padx=4, pady=2)
        r += 1

        ttk.Label(self, text="TSV таблица (пусто = stem_samples.tsv):").grid(row=r, column=0, sticky=tk.W, padx=4, pady=2)
        self.tsv_var = tk.StringVar(value="")
        ttk.Entry(self, textvariable=self.tsv_var, width=44).grid(row=r, column=1, sticky=tk.EW, padx=4, pady=2)
        r += 1

        mf = ttk.LabelFrame(self, text="voice.set / микрофон")
        mf.grid(row=r, column=0, columnspan=3, sticky=tk.EW, padx=4, pady=4)
        self.mic_rate_var = tk.StringVar(value="48000")
        self.mic_bits_var = tk.StringVar(value="24")
        self.mic_gain_var = tk.StringVar(value="3.0")
        self.chunk_var = tk.StringVar(value="512")
        self.record_gain_var = tk.StringVar(value="0.0")
        self.mic_mute_var = tk.BooleanVar(value=False)
        self.no_clip_var = tk.BooleanVar(value=False)

        rr = 0
        ttk.Label(mf, text="mic_rate_hz").grid(row=rr, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(mf, textvariable=self.mic_rate_var, width=10).grid(row=rr, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Label(mf, text="mic_bits").grid(row=rr, column=2, sticky=tk.W, padx=4, pady=2)
        ttk.Combobox(mf, textvariable=self.mic_bits_var, values=("16", "24"), width=6, state="readonly").grid(
            row=rr, column=3, sticky=tk.W, padx=4, pady=2
        )
        rr += 1
        ttk.Label(mf, text="mic_gain_db").grid(row=rr, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(mf, textvariable=self.mic_gain_var, width=10).grid(row=rr, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Label(mf, text="chunk_samples").grid(row=rr, column=2, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(mf, textvariable=self.chunk_var, width=8).grid(row=rr, column=3, sticky=tk.W, padx=4, pady=2)
        rr += 1
        ttk.Label(mf, text="record_gain_db (только файл)").grid(row=rr, column=0, sticky=tk.W, padx=4, pady=2)
        ttk.Entry(mf, textvariable=self.record_gain_var, width=10).grid(row=rr, column=1, sticky=tk.W, padx=4, pady=2)
        ttk.Checkbutton(mf, text="mute (тишина в потоке)", variable=self.mic_mute_var).grid(
            row=rr, column=2, columnspan=2, sticky=tk.W, padx=4, pady=2
        )
        rr += 1
        ttk.Checkbutton(mf, text="clip:false (--no-mic-clip)", variable=self.no_clip_var).grid(
            row=rr, column=0, columnspan=2, sticky=tk.W, padx=4, pady=2
        )
        r += 1

        bf = ttk.Frame(self)
        bf.grid(row=r, column=0, columnspan=3, pady=8)
        self.btn_start = ttk.Button(bf, text="Старт записи (voice.on)", command=self._start)
        self.btn_start.pack(side=tk.LEFT, padx=4)
        self.btn_stop = ttk.Button(bf, text="Стоп и сохранить (voice.off)", command=self._stop, state=tk.DISABLED)
        self.btn_stop.pack(side=tk.LEFT, padx=4)
        self.btn_play_local = ttk.Button(bf, text="Прослушать WAV", command=self._play_local_wav)
        self.btn_play_local.pack(side=tk.LEFT, padx=4)
        self.btn_stop_local = ttk.Button(bf, text="Стоп прослушивания", command=self._stop_local_wav)
        self.btn_stop_local.pack(side=tk.LEFT, padx=4)

        self.columnconfigure(1, weight=1)

    def _browse_out(self) -> None:
        p = filedialog_save_flac(self)
        if p:
            self.out_var.set(p)

    def _parse_mic_params(self):
        mic_rate = int(self.mic_rate_var.get().strip())
        mic_bits = int(self.mic_bits_var.get().strip())
        mic_gain = float(self.mic_gain_var.get().strip())
        chunk = int(self.chunk_var.get().strip())
        record_gain = float(self.record_gain_var.get().strip())
        if not 64 <= chunk <= 512:
            raise ValueError("chunk_samples: 64..512")
        if not 8000 <= mic_rate <= 96000:
            raise ValueError("mic_rate: 8000..96000")
        if mic_bits not in (16, 24):
            raise ValueError("mic_bits: 16 или 24")
        return mic_rate, mic_bits, mic_gain, chunk, record_gain

    def _mic_handler(self, message: Dict[str, Any]) -> None:
        if not self._recording.is_set():
            return
        pl = message.get("payload") or ""
        if not pl:
            return
        try:
            rate, block, bits = parse_mic_stream_payload(pl)
            with self._lock:
                self._sample_rate_ref[0] = rate
                self._bits_ref[0] = bits
                self._chunks.append((block, bits))
        except Exception as ex:
            self.session.log(f"[voice] parse STREAM: {ex}")

    def _noop_stream(self, _message: Dict[str, Any]) -> None:
        pass

    def _start(self) -> None:
        try:
            self.session.require_client()
            self._parse_mic_params()
        except RuntimeError as e:
            messagebox.showwarning("Микрофон", str(e))
            return
        except ValueError as e:
            messagebox.showwarning("Микрофон", str(e))
            return

        mic_rate, mic_bits, mic_gain, chunk, _ = self._parse_mic_params()

        def work():
            c = self.session.require_client()
            c.register_stream_handler(STREAM_ID_MIC, self._mic_handler)
            with self._lock:
                self._chunks.clear()
            self._recording.set()
            ok = c.send_voice_set(
                rate_hz=mic_rate,
                bits=mic_bits,
                gain_db=mic_gain,
                chunk_samples=chunk,
                mute=self.mic_mute_var.get(),
                clip=not self.no_clip_var.get(),
                command_timeout=min(10.0, self.session.timeout),
            )
            if not ok:
                self._recording.clear()
                raise RuntimeError("voice.set отклонён")
            tid = c.send_command(self.session.destination, "voice.on")
            resp = c.wait_for_response(tid, timeout=10.0)
            return resp

        def ok(resp):
            self.btn_stop.configure(state=tk.NORMAL)
            self.btn_start.configure(state=tk.DISABLED)
            prev = _preview_resp(resp)
            self.session.log(f"[voice] Запись: {prev}")

        def err(e):
            self._recording.clear()
            try:
                self.session.require_client().register_stream_handler(STREAM_ID_MIC, self._noop_stream)
            except RuntimeError:
                pass
            self.session.log(f"[voice] Старт не удался: {e}")
            messagebox.showerror("Микрофон", str(e))

        run_in_thread(self.root, work, ok, err)

    def _stop(self) -> None:
        out = Path(self.out_var.get().strip() or "voice_capture.flac")
        tsv_s = self.tsv_var.get().strip()
        table_path = Path(tsv_s) if tsv_s else out.with_name(out.stem + "_samples.tsv")
        wav_path = out.with_suffix(".wav")

        try:
            _, _, _, _, record_gain_db = self._parse_mic_params()
        except ValueError as e:
            messagebox.showwarning("Микрофон", str(e))
            return

        def work():
            self._recording.clear()
            c = self.session.require_client()
            tid = c.send_command(self.session.destination, "voice.off")
            resp = c.wait_for_response(tid, timeout=10.0)
            c.register_stream_handler(STREAM_ID_MIC, self._noop_stream)

            with self._lock:
                sr = self._sample_rate_ref[0]
                br = self._bits_ref[0]
                chunks = list(self._chunks)
            if not chunks:
                merged = np.array([], dtype=np.int32 if br == 24 else np.int16)
            else:
                if chunks[0][1] == 24:
                    merged = np.concatenate([np.asarray(c[0], dtype=np.int32) for c in chunks])
                else:
                    merged = np.concatenate([np.asarray(c[0], dtype=np.int16) for c in chunks])
            n = int(merged.shape[0])
            if n > 0:
                merged = apply_record_gain_db(merged, br, record_gain_db)
                save_audio_flac_and_wav(out, wav_path, merged, sr, br)
                write_samples_table(table_path, merged, br)
            return resp, n, sr, br, str(out), str(wav_path), str(table_path)

        def ok(result):
            self.btn_start.configure(state=tk.NORMAL)
            self.btn_stop.configure(state=tk.DISABLED)
            resp, n, sr, br, op, wp, tp = result
            self.session.log(f"[voice] Стоп: {_preview_resp(resp)}")
            if n == 0:
                self.session.status("Микрофон: нет сэмплов, файлы не созданы.")
            else:
                self._last_wav_path = Path(wp)
                self.session.log(f"[voice] Сохранено: {n} samples, {sr} Hz, {br}-bit")
                self.session.status(f"Микрофон: сохранено {n} сэмплов ({sr} Hz, {br}-bit).")

        def err(e):
            self.btn_start.configure(state=tk.NORMAL)
            self.btn_stop.configure(state=tk.DISABLED)
            self.session.log(f"[voice] Ошибка стопа: {e}")
            messagebox.showerror("Микрофон", str(e))

        run_in_thread(self.root, work, ok, err)

    def _resolve_wav_for_playback(self) -> Path:
        if self._last_wav_path and self._last_wav_path.is_file():
            return self._last_wav_path
        out = Path(self.out_var.get().strip() or "voice_capture.flac")
        wav_path = out.with_suffix(".wav")
        if wav_path.is_file():
            return wav_path
        raise FileNotFoundError("WAV файл не найден. Сначала выполните запись и сохранение.")

    def _play_local_wav(self) -> None:
        if winsound is None:
            messagebox.showwarning("Микрофон", "Локальное прослушивание доступно только в Windows.")
            return
        try:
            wav_path = self._resolve_wav_for_playback()
        except FileNotFoundError as e:
            messagebox.showwarning("Микрофон", str(e))
            return
        try:
            winsound.PlaySound(str(wav_path), winsound.SND_FILENAME | winsound.SND_ASYNC)
            self.session.log(f"[voice] Локальное прослушивание: {wav_path}")
        except Exception as e:
            self.session.log(f"[voice] Ошибка прослушивания: {e}")
            messagebox.showerror("Микрофон", f"Не удалось воспроизвести WAV:\n{e}")

    def _stop_local_wav(self) -> None:
        if winsound is None:
            return
        try:
            winsound.PlaySound(None, winsound.SND_PURGE)
            self.session.log("[voice] Локальное прослушивание остановлено")
        except Exception as e:
            self.session.log(f"[voice] Ошибка остановки прослушивания: {e}")


def filedialog_save_flac(parent) -> str | None:
    from tkinter import filedialog

    return filedialog.asksaveasfilename(
        parent=parent,
        title="Выходной FLAC",
        defaultextension=".flac",
        filetypes=[("FLAC", "*.flac"), ("Все файлы", "*.*")],
    )


def _preview_resp(resp) -> str:
    if not resp:
        return "(нет ответа)"
    pl = resp.get("payload")
    if not pl:
        return json.dumps(resp, ensure_ascii=False)[:200]
    try:
        return base64.b64decode(pl).decode("utf-8", errors="replace")
    except Exception:
        return str(pl)[:120]

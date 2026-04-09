#!/usr/bin/env python3
"""
Simple stress/smoke runner for ESP32-S3 speaker streaming.

Runs repeated playback loops and prints pass/fail counters.
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from vkorobka_client import VkorobkaClient


def main() -> int:
    p = argparse.ArgumentParser(description="Stress smoke test for dyn playback")
    p.add_argument("audio_file", type=Path)
    p.add_argument("--loops", type=int, default=10)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=1236)
    p.add_argument("--rate", type=int, default=16000)
    p.add_argument("--chunk-samples", type=int, default=512)
    p.add_argument("--pause-s", type=float, default=0.25)
    args = p.parse_args()

    if not args.audio_file.is_file():
        print(f"[smoke] file not found: {args.audio_file}")
        return 2

    cli = VkorobkaClient(server_host=args.host, server_port=args.port, timeout=10.0, verbose_udp=False)
    ok_count = 0
    fail_count = 0
    t0 = time.perf_counter()
    try:
        for i in range(args.loops):
            print(f"[smoke] loop {i + 1}/{args.loops}")
            ok = cli.play_audio_file_to_esp32_dyn(
                str(args.audio_file),
                chunk_samples=args.chunk_samples,
                pace=True,
                pace_factor=0.97,
                dyn_rate_hz=args.rate,
                dyn_bits=16,
                send_dyn_set=True,
                flow_control=True,
                adaptive_pace=True,
            )
            if ok:
                ok_count += 1
            else:
                fail_count += 1
            time.sleep(max(0.0, args.pause_s))
    finally:
        cli.close()

    elapsed = max(0.001, time.perf_counter() - t0)
    print(
        f"[smoke] done loops={args.loops} ok={ok_count} fail={fail_count} "
        f"elapsed_s={elapsed:.2f} loops_per_min={(args.loops / elapsed) * 60.0:.2f}"
    )
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())


# Audio Streaming Profiles (ESP32-S3)

This project now includes a speech-focused default profile and a path for codec-based optimization.

## Baseline Profile (enabled now)

- Transport: PCM16 mono
- Sample rate: 16000 Hz (default in `speaker_cli.py`)
- Stream: `stream_id=3`
- Flow control: `CHUNK_ACK_AUDIO:<seq>` acknowledgements
- Adaptive pacing: enabled by default

Use this profile first for Smart Speaker scenarios where display and audio run together.

## Why 16 kHz Mono

- Lowers network payload size versus 22.05/24/48 kHz
- Good quality for voice assistant responses
- Reduces ESP32 conversion and queue pressure

## OPUS Prototype Path

The next upgrade path is to keep TCP transport and move from raw PCM payloads to OPUS payloads:

1. Encode TTS on host side (or backend) to Opus frames.
2. Deliver Opus payload chunks over `MSG_TYPE_STREAM` (new `stream_id`, e.g. `4`).
3. Decode on ESP32-S3 using either:
   - ESP-ADF Opus decoder element, or
   - `micro-opus` component integration in ESP-IDF.
4. Push decoded PCM into the same I2S playback pipeline.

Expected gain: lower network bitrate and fewer transport stalls during parallel display rendering.


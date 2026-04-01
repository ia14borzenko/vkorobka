import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import sounddevice as sd
import soundfile as sf   # для сохранения WAV (установить: pip install soundfile)

# 1. Загрузка данных
df = pd.read_csv('capture_samples.tsv', sep='\t')
samples = df['sample_int24'].values.astype(np.int32)   # исходные 24-битные значения

# 2. Преобразование в float (-1..1) для воспроизведения
# Максимальное абсолютное значение для 24-битного знакового: 2**23 - 1 ≈ 8388607
# Нормализуем, деля на 2**23, чтобы пики не превышали 1
max_abs = 2**23
audio = samples / max_abs

# 3. Параметры воспроизведения
fs = 48000          # частота дискретизации, Гц
duration = len(audio) / fs
print(f"Длительность: {duration:.2f} сек")

# 4. Воспроизведение
print("Воспроизведение...")
sd.play(audio, samplerate=fs)
sd.wait()          # ждём окончания

# 5. Сохранение в WAV (опционально)
sf.write('capture_samples.wav', audio, fs, subtype='PCM_24')
print("Сохранено в capture_samples.wav")
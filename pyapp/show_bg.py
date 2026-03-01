"""
Скрипт для вывода изображения bg.jpg на дисплей MAR3501 (ILI9486) через ESP32-S3.

Использует существующий UDP JSON API (pyapp -> win-x64) и новый STREAM-протокол
для передачи кадра RGB565.
"""

import sys

from vkorobka_client import VkorobkaClient
from image_utils import load_bg_image, image_to_rgb565_be, display_image


def main() -> int:
    print("=" * 60)
    print("VKOROBKA: Show BG image on ESP32 MAR3501 display")
    print("=" * 60)

    # Загружаем и подготавливаем фон
    print("[init] Загрузка bg.jpg и приведение к 480x320...")
    img = load_bg_image("bg.jpg")
    display_image(
        image_bytes=image_to_rgb565_be(img)[:0],  # только инфо по размерам не покажем
        title="BG image (info only)",
    )
    width, height = img.size
    print(f"[init] Готово: {width}x{height}")

    # Конвертация в RGB565 BE
    print("[init] Конвертация в RGB565 (big-endian)...")
    frame_bytes = image_to_rgb565_be(img)
    print(f"[init] Размер кадра: {len(frame_bytes)} байт")

    # Создаем клиента
    print("\n[net] Подключение к win-x64 (UDP)...")
    client = VkorobkaClient(server_host="127.0.0.1", server_port=1236, timeout=10.0)

    try:
        # Отправка кадра на ESP32
        print("\n[stream] Отправка кадра на ESP32 (destination='esp32')...")
        response = client.send_lcd_frame_rgb565(
            destination="esp32",
            frame_bytes=frame_bytes,
            width=width,
            height=height,
        )

        if response is None:
            print("[result] ❌ Не получено подтверждение от ESP32")
            return 1

        payload = response.get("payload", "")
        print(f"[result] ✅ Ответ от ESP32: type={response.get('type')}, payload={payload}")
        return 0
    except KeyboardInterrupt:
        print("\n[info] Прервано пользователем")
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())


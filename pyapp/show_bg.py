"""
Скрипт для вывода изображения bg.jpg на дисплей MAR3501 (ILI9486) через ESP32-S3.

Использует существующий UDP JSON API (pyapp -> win-x64) и новый STREAM-протокол
для передачи кадра RGB565.

ВАЖНО для ИИ-агента:
- Изображение автоматически приводится к размеру 480x320 (letterboxing)
- Конвертируется в RGB565 big-endian (исправлено зеркалирование)
- Разбивается на чанки по 20 строк (по умолчанию)
- Каждый чанк отправляется с подтверждением (flow control)
- Ожидается финальный ответ LCD_FRAME_OK от ESP32

Процесс:
1. Загрузка bg.jpg и приведение к 480x320
2. Конвертация в RGB565 big-endian (с исправлением зеркалирования)
3. Отправка через send_lcd_frame_rgb565() с подтверждениями
4. Ожидание финального ответа LCD_FRAME_OK
"""

import sys
import argparse

from vkorobka_client import VkorobkaClient
from image_utils import load_bg_image, image_to_rgb565_be, display_image


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Отправка фонового изображения на дисплей ESP32 (ILI9486) "
            "через STREAM-протокол с подтверждением чанков."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Примеры:\n"
            "  python show_bg.py\n"
            "  python show_bg.py --image bg.jpg --destination esp32 --chunk-rows 20\n"
            "  python show_bg.py --host 127.0.0.1 --port 1236 --timeout 10\n"
        ),
    )
    parser.add_argument("--image", default="bg.jpg", help="Путь к изображению (по умолчанию: bg.jpg)")
    parser.add_argument("--host", default="127.0.0.1", help="UDP host win-x64 (по умолчанию: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=1236, help="UDP порт win-x64 (по умолчанию: 1236)")
    parser.add_argument("--timeout", type=float, default=10.0, help="Таймаут клиента (сек), по умолчанию: 10.0")
    parser.add_argument("--destination", default="esp32", help="Назначение команды (по умолчанию: esp32)")
    parser.add_argument(
        "--chunk-rows",
        type=int,
        default=20,
        help="Высота чанка в строках (по умолчанию: 20)",
    )
    args = parser.parse_args()

    print("=" * 60)
    print("VKOROBKA: Show BG image on ESP32 MAR3501 display")
    print("=" * 60)

    # Загружаем и подготавливаем фон
    print(f"[init] Загрузка {args.image} и приведение к 480x320...")
    img = load_bg_image(args.image)
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
    client = VkorobkaClient(server_host=args.host, server_port=args.port, timeout=args.timeout)

    try:
        # Отправка кадра на ESP32
        print(f"\n[stream] Отправка кадра на ESP32 (destination='{args.destination}')...")
        response = client.send_lcd_frame_rgb565(
            destination=args.destination,
            frame_bytes=frame_bytes,
            width=width,
            height=height,
            chunk_rows=max(1, int(args.chunk_rows)),
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


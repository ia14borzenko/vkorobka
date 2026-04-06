"""
CLI скрипт для управления текстингом на дисплее ILI9486
"""
import sys
import argparse
from pathlib import Path
from texting import TextingManager
from vkorobka_client import VkorobkaClient
from font_utils import generate_char_images, create_char_map, ALL_CHARS


def main() -> int:
    """
    Основная функция CLI скрипта.
    """
    parser = argparse.ArgumentParser(
        description=(
            "CLI для управления текстовым полем на дисплее ESP32: "
            "очистка поля и печать текста через TEXT_CLEAR/TEXT_ADD."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Примеры использования:

  # Очистка поля
  python texting_cli.py --command clear

  # Печать текста
  python texting_cli.py --command add_text --text "Привет, мир!"

  # С параметрами
  python texting_cli.py --command add_text --text "Hello" --char-height 16 --typing-speed 100

  # С указанием шрифта
  python texting_cli.py --command add_text --text "Test" --font-path fonts/custom.ttf

  # Регенерация кэша символов + печать
  python texting_cli.py --command add_text --text "Привет" --regen-font --char-height 16

Примечания:
  - Перед запуском нужен активный win-x64 (UDP host/port).
  - По умолчанию команды отправляются в destination=esp32.
        """
    )
    
    # Основные аргументы
    parser.add_argument(
        '--command',
        type=str,
        required=True,
        choices=['clear', 'add_text'],
        help='Команда: clear (очистка поля) или add_text (печать текста)'
    )
    
    parser.add_argument(
        '--text',
        type=str,
        help='Текст для печати (требуется для команды add_text)'
    )
    
    # Параметры шрифта
    parser.add_argument(
        '--font-path',
        type=str,
        default='fonts/font.ttf',
        help='Путь к TTF/OTF шрифту (по умолчанию: fonts/font.ttf)'
    )

    parser.add_argument(
        '--regen-font',
        action='store_true',
        help='Полностью пересоздать изображения символов и карту символов перед работой'
    )
    
    # Параметры поля
    parser.add_argument(
        '--field-x',
        type=int,
        default=52,
        help='X координата начала поля (по умолчанию: 52)'
    )
    
    parser.add_argument(
        '--field-y',
        type=int,
        default=160,
        help='Y координата начала поля (по умолчанию: 160)'
    )
    
    parser.add_argument(
        '--field-width',
        type=int,
        default=380,
        help='Ширина поля в пикселях (по умолчанию: 380)'
    )
    
    parser.add_argument(
        '--field-height',
        type=int,
        default=120,
        help='Высота поля в пикселях (по умолчанию: 120)'
    )
    
    # Параметры символов
    parser.add_argument(
        '--char-height',
        type=int,
        default=14,
        help='Высота символа в пикселях (по умолчанию: 14)'
    )
    
    parser.add_argument(
        '--line-spacing',
        type=int,
        default=2,
        help='Расстояние между строками в пикселях (по умолчанию: 2)'
    )
    
    parser.add_argument(
        '--typing-speed',
        type=int,
        default=50,
        help='Скорость печати в миллисекундах на символ (по умолчанию: 50)'
    )
    
    # Параметры сети
    parser.add_argument(
        '--server-host',
        type=str,
        default='127.0.0.1',
        help='IP адрес win-x64 приложения (по умолчанию: 127.0.0.1)'
    )
    
    parser.add_argument(
        '--server-port',
        type=int,
        default=1236,
        help='UDP порт win-x64 приложения (по умолчанию: 1236)'
    )
    
    parser.add_argument(
        '--timeout',
        type=float,
        default=10.0,
        help='Таймаут ожидания ответа в секундах (по умолчанию: 10.0)'
    )
    
    parser.add_argument(
        '--destination',
        type=str,
        default='esp32',
        help='Назначение для отправки команд (по умолчанию: esp32)'
    )
    
    args = parser.parse_args()
    
    # Проверка аргументов
    if args.command == 'add_text' and not args.text:
        parser.error("--text требуется для команды add_text")
    
    # Проверка существования шрифта
    font_path = Path(args.font_path)
    if not font_path.exists():
        print(f"❌ Ошибка: шрифт не найден: {args.font_path}")
        print(f"   Убедитесь, что файл существует или укажите правильный путь через --font-path")
        return 1
    
    print("=" * 60)
    print("VKOROBKA: Texting CLI")
    print("=" * 60)
    print(f"Команда: {args.command}")
    if args.command == 'add_text':
        print(f"Текст: '{args.text}'")
    print(f"Шрифт: {args.font_path}")
    print(f"Поле: ({args.field_x}, {args.field_y}), размер: {args.field_width}x{args.field_height}")
    print(f"Символы: высота={args.char_height}px, межстрочное расстояние={args.line_spacing}px")
    print(f"Скорость: {args.typing_speed} мс/символ")
    print()
    
    # Создаем клиент
    print(f"[net] Подключение к win-x64 ({args.server_host}:{args.server_port})...")
    try:
        client = VkorobkaClient(
            server_host=args.server_host,
            server_port=args.server_port,
            timeout=args.timeout
        )
    except Exception as e:
        print(f"❌ Ошибка при создании клиента: {e}")
        return 1
    
    # Определяем пути для символов
    fonts_dir = font_path.parent
    chars_dir = fonts_dir / "chars"
    char_map_json = fonts_dir / "char_map.json"

    # При необходимости полностью пересоздаём картинки и карту символов
    if args.regen_font:
        print("[font] Полная регенерация изображений символов и карты (по флагу --regen-font)...")
        try:
            generate_char_images(
                font_path=str(font_path),
                output_dir=str(chars_dir),
                char_height=args.char_height,
                chars_to_generate=ALL_CHARS,
            )
            create_char_map(str(chars_dir), str(char_map_json))
            print("[font] Регенерация символов и карты завершена")
        except Exception as e:
            print(f"❌ Ошибка при регенерации символов: {e}")
            client.close()
            return 1
    
    # Создаем менеджер текстинга
    try:
        texting_manager = TextingManager(
            field_x=args.field_x,
            field_y=args.field_y,
            field_width=args.field_width,
            field_height=args.field_height,
            char_height=args.char_height,
            line_spacing=args.line_spacing,
            typing_speed_ms=args.typing_speed,
            font_path=str(font_path),
            chars_dir=str(chars_dir),
            char_map_json=str(char_map_json),
            client=client,
            destination=args.destination
        )
    except Exception as e:
        print(f"❌ Ошибка при инициализации TextingManager: {e}")
        import traceback
        traceback.print_exc()
        client.close()
        return 1
    
    # Выполняем команду
    try:
        if args.command == 'clear':
            print("\n[command] Выполнение команды: clear")
            texting_manager.clear_field()
            print("✅ Поле очищено")
        
        elif args.command == 'add_text':
            print(f"\n[command] Выполнение команды: add_text")
            print(f"[command] Текст: '{args.text}'")
            texting_manager.add_text(args.text)
            print("✅ Текст напечатан")
        
    except KeyboardInterrupt:
        print("\n\n[info] Прервано пользователем")
        return 1
    except Exception as e:
        print(f"\n❌ Ошибка при выполнении команды: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        client.close()
    
    return 0


if __name__ == '__main__':
    sys.exit(main())

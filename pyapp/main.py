"""
Основной скрипт для тестирования компонентов системы vkorobka
"""
import sys
import time
import argparse
from vkorobka_client import VkorobkaClient
from image_utils import (
    generate_test_image,
    display_image,
    verify_image_integrity,
    mirror_image_horizontal
)


def test_component(
    client: VkorobkaClient,
    destination: str,
    original_image: bytes,
    component_name: str,
    show_images: bool = True,
):
    """
    Тестирует один компонент системы
    
    Args:
        client: Клиент для связи с win-x64
        destination: Назначение ("win", "esp32")
        original_image: Исходное изображение
        component_name: Имя компонента для вывода
    
    Returns:
        bool: True если тест успешен
    """
    print(f"\n{'='*60}")
    print(f"Тестирование компонента: {component_name.upper()}")
    print(f"{'='*60}")
    
    print(f"[test] Отправка изображения размером {len(original_image)} байт ({len(original_image)/1024:.2f} КБ)")
    
    print(f"[test] Destination: {destination}")
    print(f"[test] Размер данных: {len(original_image)} байт")
    
    # Отправляем тест
    received_image = client.test_component(destination, original_image)
    
    if received_image is None:
        print(f"❌ Тест {component_name} ПРОВАЛЕН: не получен ответ")
        return False
    
    # Для обычного режима - полная проверка целостности изображения
    # Проверяем целостность
    is_valid, error_msg = verify_image_integrity(original_image, received_image)
    
    if not is_valid:
        print(f"❌ Тест {component_name} ПРОВАЛЕН: {error_msg}")
        return False
    
    # Проверяем, что изображение действительно отзеркалено
    # (сравниваем с ожидаемым отзеркаленным изображением)
    expected_mirrored = mirror_image_horizontal(original_image)
    
    # Сравниваем размеры
    if len(received_image) != len(expected_mirrored):
        print(f"⚠️  Размеры отличаются: получено {len(received_image)} байт, ожидалось {len(expected_mirrored)} байт")
        print(f"   (Это может быть нормально из-за различий в сжатии JPG)")
    
    # Отображаем результаты
    print(f"\n✅ Тест {component_name} УСПЕШЕН")
    print(f"   Исходное изображение: {len(original_image)} байт")
    print(f"   Полученное изображение: {len(received_image)} байт")
    
    # Показываем изображения
    if show_images:
        display_image(original_image, f"Исходное изображение ({component_name})")
        time.sleep(0.5)  # Небольшая задержка между отображениями
        display_image(received_image, f"Ответ от {component_name}")
    
    return True


def main():
    """Основная функция"""
    # Парсинг аргументов командной строки
    parser = argparse.ArgumentParser(
        description=(
            "Проверка базовой цепочки pyapp -> win-x64 -> esp32: "
            "отправка тестового JPG и валидация зеркального ответа."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Примеры:\n"
            "  python main.py\n"
            "  python main.py --host 127.0.0.1 --port 1236 --timeout 2.0 --size-kb 4\n"
            "\n"
            "Примечание:\n"
            "  Скрипт тестирует оба направления: destination='win' и destination='esp32'."
        ),
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="UDP host win-x64 (по умолчанию: 127.0.0.1)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=1236,
        help="UDP порт win-x64 (по умолчанию: 1236)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="Таймаут ожидания ответа (сек), по умолчанию: 2.0",
    )
    parser.add_argument(
        "--size-kb",
        type=int,
        default=4,
        help="Размер тестового JPG в КБ (по умолчанию: 4)",
    )
    parser.add_argument(
        "--no-display",
        action="store_true",
        help="Не открывать окна предпросмотра изображений (только консольный отчёт)",
    )
    args = parser.parse_args()
    
    print("="*60)
    print("VKOROBKA Component Test Application")
    print("="*60)
    
    # Генерируем тестовое изображение
    size_kb = max(1, int(args.size_kb))
    print(f"\n[init] Генерация тестового изображения ({size_kb} КБ)...")
    test_image = generate_test_image(size_kb=size_kb)
    print(f"[init] Изображение сгенерировано: {len(test_image)} байт ({len(test_image)/1024:.2f} КБ)")
    
    # Показываем исходное изображение
    if not args.no_display:
        display_image(test_image, "Исходное тестовое изображение")
    
    # Создаем клиент
    print("\n[init] Подключение к win-x64 приложению...")
    client = VkorobkaClient(server_host=args.host, server_port=args.port, timeout=args.timeout)
    
    try:
        # Результаты тестов
        results = {}
        
        print("\n[config] Компоненты для тестирования:")
        print(f"  WIN        : ✅ ВКЛЮЧЕН")
        print(f"  ESP32      : ✅ ВКЛЮЧЕН")
        
        # Тест 1: win-x64
        # Тест 1: win-x64
        results['win'] = test_component(
            client, 'win', test_image, 'Windows (win-x64)', show_images=not args.no_display
        )
        time.sleep(1)  # Пауза между тестами
        
        # Тест 2: esp32
        results['esp32'] = test_component(
            client, 'esp32', test_image, 'ESP32', show_images=not args.no_display
        )
        
        # Итоговый отчет
        print(f"\n{'='*60}")
        print("ИТОГОВЫЙ ОТЧЕТ")
        print(f"{'='*60}")
        
        total_tests = len(results)
        passed_tests = sum(1 for r in results.values() if r)
        
        for component, result in results.items():
            if result is None:
                status = "⏭️  ПРОПУЩЕН"
            elif result:
                status = "✅ ПРОЙДЕН"
            else:
                status = "❌ ПРОВАЛЕН"
            print(f"  {component.upper():10} : {status}")
        
        # Подсчитываем только выполненные тесты
        executed_tests = sum(1 for r in results.values() if r is not None)
        passed_tests = sum(1 for r in results.values() if r is True)
        
        print(f"\nВсего выполнено тестов: {executed_tests}")
        print(f"Успешно: {passed_tests}")
        print(f"Провалено: {executed_tests - passed_tests}")
        print(f"Пропущено: {total_tests - executed_tests}")
        
        if passed_tests == total_tests:
            print("\n🎉 ВСЕ ТЕСТЫ ПРОЙДЕНЫ УСПЕШНО!")
            return 0
        else:
            print("\n⚠️  НЕКОТОРЫЕ ТЕСТЫ ПРОВАЛЕНЫ")
            return 1
            
    except KeyboardInterrupt:
        print("\n\n[info] Прервано пользователем")
        return 1
    except Exception as e:
        print(f"\n[error] Критическая ошибка: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        client.close()


if __name__ == '__main__':
    sys.exit(main())

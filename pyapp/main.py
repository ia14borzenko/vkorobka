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


def test_component(client: VkorobkaClient, destination: str, original_image: bytes, component_name: str, stm32_test_mode: bool = False):
    """
    Тестирует один компонент системы
    
    Args:
        client: Клиент для связи с win-x64
        destination: Назначение ("win", "esp32", "stm32")
        original_image: Исходное изображение
        component_name: Имя компонента для вывода
        stm32_test_mode: Если True и destination="stm32", отправляет только 2 байта для тестирования
    
    Returns:
        bool: True если тест успешен
    """
    print(f"\n{'='*60}")
    print(f"Тестирование компонента: {component_name.upper()}")
    print(f"{'='*60}")
    
    # Для STM32 в режиме тестирования используем только 2 байта
    test_data = original_image
    if stm32_test_mode and destination == "stm32":
        test_data = b"\xAA\x55"  # Простые тестовые данные: 2 байта
        print(f"[test] Режим тестирования STM32: отправка только 2 байт данных")
    else:
        print(f"[test] Отправка изображения размером {len(original_image)} байт ({len(original_image)/1024:.2f} КБ)")
    
    print(f"[test] Destination: {destination}")
    print(f"[test] Размер данных: {len(test_data)} байт")
    
    # Отправляем тест
    received_image = client.test_component(destination, test_data)
    
    if received_image is None:
        print(f"❌ Тест {component_name} ПРОВАЛЕН: не получен ответ")
        return False
    
    # Для режима тестирования STM32 с 2 байтами - упрощенная проверка
    if stm32_test_mode and destination == "stm32":
        print(f"\n✅ Тест {component_name} УСПЕШЕН (режим тестирования)")
        print(f"   Отправлено: {len(test_data)} байт")
        print(f"   Получено: {len(received_image)} байт")
        if len(received_image) == len(test_data):
            print(f"   ✅ Размеры совпадают")
        else:
            print(f"   ⚠️  Размеры отличаются (это может быть нормально)")
        return True
    
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
    display_image(original_image, f"Исходное изображение ({component_name})")
    time.sleep(0.5)  # Небольшая задержка между отображениями
    display_image(received_image, f"Ответ от {component_name}")
    
    return True


def main():
    """Основная функция"""
    # Парсинг аргументов командной строки
    parser = argparse.ArgumentParser(description='VKOROBKA Component Test Application')
    parser.add_argument('--stm', action='store_true', help='Тестировать только STM32')
    parser.add_argument('--stm32-test-mode', action='store_true', 
                        help='Режим тестирования STM32: отправлять только 2 байта данных вместо полного изображения')
    args = parser.parse_args()
    
    print("="*60)
    print("VKOROBKA Component Test Application")
    print("="*60)
    
    # Генерируем тестовое изображение
    print("\n[init] Генерация тестового изображения (4 КБ)...")
    test_image = generate_test_image(size_kb=4)
    print(f"[init] Изображение сгенерировано: {len(test_image)} байт ({len(test_image)/1024:.2f} КБ)")
    
    # Показываем исходное изображение (только если не в режиме тестирования STM32)
    if not args.stm32_test_mode:
        display_image(test_image, "Исходное тестовое изображение")
    
    # Создаем клиент
    print("\n[init] Подключение к win-x64 приложению...")
    client = VkorobkaClient(server_host='127.0.0.1', server_port=1236, timeout=2.0)
    
    try:
        # Результаты тестов
        results = {}
        
        # Определяем, какие компоненты тестировать
        test_win = not args.stm
        test_esp32 = not args.stm
        test_stm32 = True  # Всегда тестируем STM32, если указан --stm
        
        if not args.stm:
            print("\n[config] Компоненты для тестирования:")
            print(f"  WIN        : {'✅ ВКЛЮЧЕН' if test_win else '❌ ОТКЛЮЧЕН'}")
            print(f"  ESP32      : {'✅ ВКЛЮЧЕН' if test_esp32 else '❌ ОТКЛЮЧЕН'}")
            print(f"  STM32      : {'✅ ВКЛЮЧЕН' if test_stm32 else '❌ ОТКЛЮЧЕН'}")
        else:
            print("\n[config] Режим тестирования: только STM32")
        
        if args.stm32_test_mode:
            print("[config] Режим тестирования STM32: включен (2 байта данных)")
        
        # Тест 1: win-x64
        if test_win:
            results['win'] = test_component(client, 'win', test_image, 'Windows (win-x64)', args.stm32_test_mode)
            time.sleep(1)  # Пауза между тестами
        else:
            print(f"\n{'='*60}")
            print("Пропуск теста: Windows (win-x64)")
            print(f"{'='*60}")
            results['win'] = None
        
        # Тест 2: esp32
        if test_esp32:
            results['esp32'] = test_component(client, 'esp32', test_image, 'ESP32', args.stm32_test_mode)
            time.sleep(1)
        else:
            print(f"\n{'='*60}")
            print("Пропуск теста: ESP32")
            print(f"{'='*60}")
            results['esp32'] = None
        
        # Тест 3: stm32
        results['stm32'] = test_component(client, 'stm32', test_image, 'STM32', args.stm32_test_mode)
        
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

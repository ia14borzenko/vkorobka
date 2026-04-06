"""
Модуль текстинга: управление текстовым полем, разбивка текста на строки, управление курсором
"""
import base64
from typing import Dict, Tuple, List, Optional
from PIL import Image
from font_utils import (
    load_font_cache,
    get_char_image,
    get_char_width,
    ensure_font_ready,
    ALL_CHARS
)
from vkorobka_client import VkorobkaClient
from image_utils import image_to_rgb565_be


class TextingManager:
    """
    Менеджер текстинга: управление текстовым полем на дисплее.
    """
    
    def __init__(
        self,
        field_x: int = 52,
        field_y: int = 160,
        field_width: int = 380,
        field_height: int = 120,
        char_height: int = 14,
        line_spacing: int = 2,
        typing_speed_ms: int = 50,
        font_path: str = "fonts/font.ttf",
        chars_dir: str = "fonts/chars",
        char_map_json: str = "fonts/char_map.json",
        client: Optional[VkorobkaClient] = None,
        destination: str = "esp32",
        command_timeout_s: float = 4.0,
    ):
        """
        Инициализация менеджера текстинга.
        
        Args:
            field_x: X координата начала поля
            field_y: Y координата начала поля
            field_width: Ширина поля в пикселях
            field_height: Высота поля в пикселях
            char_height: Высота символа в пикселях
            line_spacing: Расстояние между строками в пикселях
            typing_speed_ms: Задержка между символами в миллисекундах
            font_path: Путь к TTF/OTF шрифту
            chars_dir: Директория с PNG символами
            char_map_json: Путь к JSON маппингу символов
            client: Клиент VkorobkaClient (если None, создается новый)
            destination: Назначение для отправки команд ("esp32")
        """
        self.field_x = field_x
        self.field_y = field_y
        self.field_width = field_width
        self.field_height = field_height
        self.char_height = char_height
        self.line_spacing = line_spacing
        self.typing_speed_ms = typing_speed_ms
        
        # Убеждаемся, что шрифт готов
        ensure_font_ready(font_path, chars_dir, char_map_json, char_height)
        
        # Загружаем кэш символов
        self.font_cache = load_font_cache(font_path, chars_dir, char_map_json)
        
        # Клиент для отправки команд
        self.client = client
        self.destination = destination
        self.command_timeout_s = float(command_timeout_s)
        
        # Состояние курсора
        self.cursor_x = 0  # Относительно начала поля
        self.cursor_y = 0  # Относительно начала поля
        self.cursor_visible = True
        
        # Вычисляем максимальное количество строк
        self.max_lines = (field_height + line_spacing) // (char_height + line_spacing)
        
        # Текущие строки текста (для отслеживания переполнения)
        self.current_lines: List[str] = []
        self.current_line_index = 0  # Индекс текущей строки (0 = первая строка)
        
        print(f"[texting] Инициализирован TextingManager:")
        print(f"  Поле: ({field_x}, {field_y}), размер: {field_width}x{field_height}")
        print(f"  Символы: высота={char_height}px, межстрочное расстояние={line_spacing}px")
        print(f"  Скорость: {typing_speed_ms} мс/символ")
        print(f"  Максимум строк: {self.max_lines}")
    
    def clear_field(self) -> None:
        """
        Очистка поля (заливка белым цветом).
        """
        print(f"[texting] Очистка поля ({self.field_x}, {self.field_y}, {self.field_width}x{self.field_height})")
        
        # Сбрасываем состояние курсора
        self.cursor_x = 0
        self.cursor_y = 0
        self.current_lines = []
        self.current_line_index = 0
        
        # Отправляем команду очистки (заглушка для будущей реализации на ESP32)
        if self.client:
            self._send_clear_command()
        else:
            print("[texting] Клиент не установлен, команда очистки не отправлена")
    
    def add_text(self, text: str) -> None:
        """
        Добавление текста. Текст отправляется целиком на ESP32, которая сама имитирует текстинг.
        
        Args:
            text: Текст для печати
        """
        print(f"[texting] Добавление текста: '{text}'")
        
        # Разбиваем текст на строки с учетом ширины символов (для информации)
        lines = self._split_into_lines(text)
        
        print(f"[texting] Текст разбит на {len(lines)} строк:")
        for i, line in enumerate(lines):
            print(f"  Строка {i+1}: '{line}'")
        
        # Отправляем весь текст сразу на ESP32
        if self.client:
            self._send_text_command(text, lines)
        else:
            print("[texting] Клиент не установлен, команда не отправлена")
    
    def _split_into_lines(self, text: str) -> List[str]:
        """
        Разбивает текст на строки с учетом ширины символов.
        
        Args:
            text: Исходный текст
        
        Returns:
            List[str]: Список строк
        """
        lines = []
        words = text.split(' ')
        
        current_line = ""
        current_line_width = 0
        
        for word in words:
            # Вычисляем ширину слова
            word_width = sum(get_char_width(c, self.font_cache) for c in word)
            
            # Если это не первое слово в строке, добавляем пробел
            if current_line:
                space_width = get_char_width(' ', self.font_cache)
                if current_line_width + space_width + word_width <= self.field_width:
                    current_line += ' '
                    current_line_width += space_width
                else:
                    # Текущая строка заполнена, начинаем новую
                    lines.append(current_line)
                    current_line = ""
                    current_line_width = 0
            
            # Проверяем, влезает ли слово в текущую строку
            if word_width <= self.field_width:
                # Слово влезает целиком
                if current_line_width + word_width <= self.field_width:
                    current_line += word
                    current_line_width += word_width
                else:
                    # Строка заполнена, начинаем новую
                    if current_line:
                        lines.append(current_line)
                    current_line = word
                    current_line_width = word_width
            else:
                # Слово не влезает в строку - разбиваем по символам
                if current_line:
                    lines.append(current_line)
                    current_line = ""
                    current_line_width = 0
                
                # Разбиваем слово на символы
                for char in word:
                    char_width = get_char_width(char, self.font_cache)
                    if current_line_width + char_width <= self.field_width:
                        current_line += char
                        current_line_width += char_width
                    else:
                        # Строка заполнена
                        if current_line:
                            lines.append(current_line)
                        current_line = char
                        current_line_width = char_width
        
        # Добавляем последнюю строку, если она не пустая
        if current_line:
            lines.append(current_line)
        
        return lines
    
    
    def _handle_overflow(self) -> None:
        """
        Обрабатывает переполнение поля: очищает первую строку и начинает печать с неё.
        (Вызывается на ESP32 при переполнении)
        """
        print(f"[texting] Переполнение поля, очистка первой строки")
        
        # Сбрасываем курсор на первую строку
        self.cursor_x = 0
        self.cursor_y = 0
        self.current_line_index = 0
        
        # Очищаем список строк (начинаем заново)
        self.current_lines = []
    
    def _send_clear_command(self) -> None:
        """
        Отправляет команду очистки поля на ESP32.
        """
        if not self.client:
            print("[texting] Клиент не установлен, команда очистки не отправлена")
            return
        
        # Для TEXT_CLEAR отправляем простой текст команды с параметрами через JSON
        payload_data = {
            "field_x": self.field_x,
            "field_y": self.field_y,
            "field_width": self.field_width,
            "field_height": self.field_height
        }
        
        # Отправляем команду
        test_id = self.client.send_command(
            destination=self.destination,
            command="TEXT_CLEAR",
            payload_data=payload_data
        )
        resp = self.client.wait_for_response(test_id, timeout=self.command_timeout_s)
        if resp is None:
            print("[texting] Предупреждение: timeout ожидания TEXT_CLEAR")
        
        print(f"[texting] Отправлена команда TEXT_CLEAR (test_id={test_id})")
    
    def _send_text_command(self, text: str, lines: List[str]) -> None:
        """
        Отправляет команду добавления текста на ESP32 вместе с изображениями символов.
        
        Args:
            text: Исходный текст
            lines: Разбитый на строки текст (для информации)
        """
        if not self.client:
            print("[texting] Клиент не установлен, команда не отправлена")
            return
        
        # Собираем уникальные символы из текста
        unique_chars = set(text)
        chars_data = {}
        
        print(f"[texting] Подготовка {len(unique_chars)} уникальных символов для передачи...")
        
        for char in unique_chars:
            # Получаем изображение символа
            char_image = get_char_image(char, self.font_cache)
            if char_image is None:
                print(f"[texting] Предупреждение: символ '{char}' (U+{ord(char):04X}) не найден в кэше, пропускаем")
                continue
            
            # Конвертируем в RGB565
            # Сначала конвертируем RGBA в RGB (белый фон для прозрачных пикселей)
            if char_image.mode == 'RGBA':
                # Создаем белый фон
                rgb_img = Image.new('RGB', char_image.size, (255, 255, 255))
                # Накладываем символ на белый фон
                rgb_img.paste(char_image, mask=char_image.split()[3])  # используем альфа-канал как маску
            else:
                rgb_img = char_image.convert('RGB')
            
            # Конвертируем в RGB565 big-endian
            rgb565_data = image_to_rgb565_be(rgb_img)
            
            # Кодируем в base64 для передачи в JSON
            char_data_b64 = base64.b64encode(rgb565_data).decode('utf-8')
            
            # Сохраняем данные символа
            unicode_code = f"U+{ord(char):04X}"
            chars_data[unicode_code] = {
                "width": char_image.width,
                "height": char_image.height,
                "data": char_data_b64
            }
        
        print(f"[texting] Подготовлено {len(chars_data)} символов (размер данных: ~{sum(len(c['data']) for c in chars_data.values())} байт base64)")
        
        # Формируем payload команды
        payload_data = {
            "text": text,
            "field_x": self.field_x,
            "field_y": self.field_y,
            "field_width": self.field_width,
            "field_height": self.field_height,
            "char_height": self.char_height,
            "line_spacing": self.line_spacing,
            "typing_speed_ms": self.typing_speed_ms,
            "cursor_x": self.cursor_x,
            "cursor_y": self.cursor_y,
            "current_line_index": self.current_line_index,
            "chars": chars_data  # Словарь с данными символов
        }
        
        # Отправляем команду
        test_id = self.client.send_command(
            destination=self.destination,
            command="TEXT_ADD",
            payload_data=payload_data
        )
        resp = self.client.wait_for_response(test_id, timeout=self.command_timeout_s)
        if resp is None:
            print("[texting] Предупреждение: timeout ожидания TEXT_ADD")
        
        print(f"[texting] Отправлена команда TEXT_ADD (test_id={test_id}), текст: '{text[:50]}...' (если длинный), символов: {len(chars_data)}")

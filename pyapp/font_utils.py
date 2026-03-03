"""
Утилиты для работы со шрифтами и генерации изображений символов
"""
import json
from pathlib import Path
from typing import Dict, Optional, Tuple, List
from PIL import Image, ImageFont, ImageDraw


# Набор символов для генерации
# Добавляем отдельно буквы Ё/ё, которые находятся вне основного диапазона А-Я/а-я
CYRILLIC_UPPER = ['Ё'] + [chr(c) for c in range(0x0410, 0x0430)]  # Ё, А-Я
CYRILLIC_LOWER = ['ё'] + [chr(c) for c in range(0x0430, 0x0450)]  # ё, а-я
LATIN_UPPER = [chr(c) for c in range(0x0041, 0x005B)]  # A-Z
LATIN_LOWER = [chr(c) for c in range(0x0061, 0x007B)]  # a-z
DIGITS = [chr(c) for c in range(0x0030, 0x003A)]  # 0-9
PUNCTUATION = [
    ' ',  # пробел (U+0020)
    '.',  # точка (U+002E)
    ',',  # запятая (U+002C)
    ':',  # двоеточие (U+003A)
    ';',  # точка с запятой (U+003B)
    '!',  # восклицательный знак (U+0021)
    '?',  # вопросительный знак (U+003F)
    '-',  # дефис (U+002D)
    '—',  # тире (U+2014)
    '–',  # короткое тире (U+2013)
    '(',  # открывающая скобка (U+0028)
    ')',  # закрывающая скобка (U+0029)
    '[',  # открывающая квадратная скобка (U+005B)
    ']',  # закрывающая квадратная скобка (U+005D)
    '{',  # открывающая фигурная скобка (U+007B)
    '}',  # закрывающая фигурная скобка (U+007D)
    '"',  # кавычки (U+0022)
    "'",  # апостроф (U+0027)
    '«',  # открывающие кавычки (U+00AB)
    '»',  # закрывающие кавычки (U+00BB)
    '…',  # многоточие (U+2026)
    '/',  # слэш (U+002F)
    '\\', # обратный слэш (U+005C)
    '|',  # вертикальная черта (U+007C)
    '+',  # плюс (U+002B)
    '=',  # равно (U+003D)
    '*',  # звездочка (U+002A)
    '%',  # процент (U+0025)
    '$',  # доллар (U+0024)
    '@',  # собака (U+0040)
    '#',  # решетка (U+0023)
    '&',  # амперсанд (U+0026)
    '^',  # крышка (U+005E)
    '~',  # тильда (U+007E)
    '`',  # обратная кавычка (U+0060)
]

ALL_CHARS = CYRILLIC_UPPER + CYRILLIC_LOWER + LATIN_UPPER + LATIN_LOWER + DIGITS + PUNCTUATION


def get_char_filename(char: str) -> str:
    """
    Получает имя файла для символа.

    Начиная с текущей версии, для всех символов используем их Unicode-код
    в имени файла, чтобы гарантировать уникальность и избежать проблем
    с регистром на файловых системах (например, Windows).

    Args:
        char: Символ (одна строка)

    Returns:
        str: Имя файла, например U0041.png для 'A', U0430.png для 'а', U0020.png для пробела.
    """
    return f"U{ord(char):04X}.png"


def generate_char_images(
    font_path: str,
    output_dir: str,
    char_height: int = 14,
    chars_to_generate: Optional[List[str]] = None
) -> None:
    """
    Генерирует PNG изображения для набора символов из TTF/OTF шрифта.
    
    Args:
        font_path: Путь к TTF/OTF файлу шрифта
        output_dir: Директория для сохранения PNG файлов
        char_height: Высота символа в пикселях (по умолчанию 14)
        chars_to_generate: Список символов для генерации (если None, используется ALL_CHARS)
    """
    if chars_to_generate is None:
        chars_to_generate = ALL_CHARS
    
    # Создаем директорию, если её нет
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    # Загружаем шрифт
    try:
        # Пробуем разные размеры шрифта, чтобы получить нужную высоту
        font_size = char_height
        font = ImageFont.truetype(font_path, font_size)
        
        # Проверяем реальную высоту и корректируем, если нужно
        test_img = Image.new('RGBA', (100, char_height * 2), (255, 255, 255, 0))
        test_draw = ImageDraw.Draw(test_img)
        test_bbox = test_draw.textbbox((0, 0), 'A', font=font)
        actual_height = test_bbox[3] - test_bbox[1]
        
        if actual_height != char_height:
            # Корректируем размер шрифта пропорционально
            font_size = int(font_size * (char_height / actual_height))
            font = ImageFont.truetype(font_path, font_size)
    except Exception as e:
        raise ValueError(f"Не удалось загрузить шрифт из {font_path}: {e}")
    
    print(f"[font_utils] Генерация символов из шрифта {font_path}")
    print(f"[font_utils] Высота символа: {char_height}px, размер шрифта: {font_size}")
    
    generated_count = 0
    
    for char in chars_to_generate:
        try:
            # Получаем размеры символа
            test_img = Image.new('RGBA', (200, char_height * 2), (255, 255, 255, 0))
            test_draw = ImageDraw.Draw(test_img)
            bbox = test_draw.textbbox((0, 0), char, font=font)
            
            char_width = bbox[2] - bbox[0]
            char_height_actual = bbox[3] - bbox[1]
            
            # Создаем изображение с прозрачным фоном
            # Добавляем небольшой отступ для символов, которые могут выходить за границы
            padding = 2
            img = Image.new('RGBA', (char_width + padding * 2, char_height + padding * 2), (255, 255, 255, 0))
            draw = ImageDraw.Draw(img)
            
            # Рисуем символ черным цветом
            # Корректируем позицию, чтобы символ был по центру по вертикали
            y_offset = (char_height + padding * 2 - char_height_actual) // 2 - bbox[1]
            draw.text((padding, y_offset), char, fill=(0, 0, 0, 255), font=font)
            
            # Обрезаем изображение, убирая лишние прозрачные области
            # Находим реальные границы символа
            bbox_img = img.getbbox()
            if bbox_img:
                img = img.crop(bbox_img)
            
            # Сохраняем PNG
            filename = get_char_filename(char)
            filepath = output_path / filename
            img.save(filepath, 'PNG')
            
            generated_count += 1
            
        except Exception as e:
            print(f"[font_utils] Ошибка при генерации символа '{char}' (U+{ord(char):04X}): {e}")
            continue
    
    print(f"[font_utils] Сгенерировано {generated_count} символов в {output_dir}")


def create_char_map(chars_dir: str, output_json: str) -> None:
    """
    Создает JSON файл с маппингом Unicode кода символа на имя файла.
    
    Args:
        chars_dir: Директория с PNG файлами символов
        output_json: Путь к выходному JSON файлу
    """
    chars_path = Path(chars_dir)
    if not chars_path.exists():
        raise ValueError(f"Директория {chars_dir} не существует")
    
    char_map = {}
    
    # Проходим по всем символам и создаем маппинг
    for char in ALL_CHARS:
        filename = get_char_filename(char)
        filepath = chars_path / filename
        
        if filepath.exists():
            unicode_code = f"U+{ord(char):04X}"
            char_map[unicode_code] = filename
    
    # Сохраняем JSON
    output_path = Path(output_json)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(char_map, f, ensure_ascii=False, indent=2)
    
    print(f"[font_utils] Создан маппинг символов: {len(char_map)} символов -> {output_json}")


def load_font_cache(
    font_path: str,
    chars_dir: str,
    char_map_json: str
) -> Dict[str, Tuple[Image.Image, int]]:
    """
    Предзагружает все символы в память при инициализации.
    
    Args:
        font_path: Путь к TTF/OTF файлу шрифта (для проверки, что шрифт существует)
        chars_dir: Директория с PNG файлами символов
        char_map_json: Путь к JSON файлу с маппингом Unicode -> имя файла
    
    Returns:
        Dict[str, Tuple[Image.Image, int]]: Словарь {unicode_code: (image, width)}
            где unicode_code в формате "U+0041", image - PIL Image, width - ширина в пикселях
    """
    # Проверяем, что шрифт существует
    if not Path(font_path).exists():
        raise ValueError(f"Шрифт не найден: {font_path}")
    
    chars_path = Path(chars_dir)
    if not chars_path.exists():
        raise ValueError(f"Директория символов не найдена: {chars_dir}")
    
    map_path = Path(char_map_json)
    if not map_path.exists():
        raise ValueError(f"Файл маппинга не найден: {char_map_json}")
    
    # Загружаем маппинг
    with open(map_path, 'r', encoding='utf-8') as f:
        char_map = json.load(f)
    
    font_cache = {}
    loaded_count = 0
    
    print(f"[font_utils] Загрузка символов в память из {chars_dir}...")
    
    for unicode_code, filename in char_map.items():
        filepath = chars_path / filename
        
        if not filepath.exists():
            print(f"[font_utils] Предупреждение: файл {filename} не найден для {unicode_code}")
            continue
        
        try:
            # Загружаем изображение
            img = Image.open(filepath)
            
            # Конвертируем в RGBA, если нужно
            if img.mode != 'RGBA':
                img = img.convert('RGBA')
            
            # Получаем ширину символа
            width = img.width
            
            font_cache[unicode_code] = (img, width)
            loaded_count += 1
            
        except Exception as e:
            print(f"[font_utils] Ошибка при загрузке {filename} ({unicode_code}): {e}")
            continue
    
    print(f"[font_utils] Загружено {loaded_count} символов в память")
    
    return font_cache


def get_char_image(char_unicode: str, font_cache: Dict[str, Tuple[Image.Image, int]]) -> Optional[Image.Image]:
    """
    Получает изображение символа по Unicode коду.
    
    Args:
        char_unicode: Unicode код в формате "U+0041" или символ (строка)
        font_cache: Кэш символов из load_font_cache()
    
    Returns:
        PIL.Image или None, если символ не найден
    """
    # Если передан символ, конвертируем в Unicode код
    if len(char_unicode) == 1:
        unicode_code = f"U+{ord(char_unicode):04X}"
    else:
        unicode_code = char_unicode
    
    if unicode_code not in font_cache:
        return None
    
    # Возвращаем копию изображения (чтобы не изменять оригинал)
    return font_cache[unicode_code][0].copy()


def get_char_width(char_unicode: str, font_cache: Dict[str, Tuple[Image.Image, int]]) -> int:
    """
    Получает ширину символа по Unicode коду.
    
    Args:
        char_unicode: Unicode код в формате "U+0041" или символ (строка)
        font_cache: Кэш символов из load_font_cache()
    
    Returns:
        int: Ширина символа в пикселях, или 0 если символ не найден
    """
    # Если передан символ, конвертируем в Unicode код
    if len(char_unicode) == 1:
        unicode_code = f"U+{ord(char_unicode):04X}"
    else:
        unicode_code = char_unicode
    
    if unicode_code not in font_cache:
        return 0
    
    return font_cache[unicode_code][1]


def ensure_font_ready(font_path: str, chars_dir: str, char_map_json: str, char_height: int = 14) -> None:
    """
    Убеждается, что шрифт готов к использованию: генерирует символы, если нужно.
    
    Args:
        font_path: Путь к TTF/OTF файлу шрифта
        chars_dir: Директория с PNG файлами символов
        char_map_json: Путь к JSON файлу с маппингом
        char_height: Высота символа в пикселях
    """
    font_file = Path(font_path)
    if not font_file.exists():
        raise ValueError(f"Шрифт не найден: {font_path}")
    
    chars_path = Path(chars_dir)
    map_path = Path(char_map_json)
    
    # Проверяем, нужно ли генерировать символы
    need_generate = False
    
    if not chars_path.exists() or not map_path.exists():
        need_generate = True
    else:
        # Проверяем, что все символы из ALL_CHARS присутствуют в маппинге
        # и соответствующие файлы реально существуют
        with open(map_path, 'r', encoding='utf-8') as f:
            char_map = json.load(f)
        
        # 1) проверяем существующие записи маппинга
        for unicode_code, filename in char_map.items():
            if not (chars_path / filename).exists():
                need_generate = True
                break
        # 2) проверяем, что для каждого символа из ALL_CHARS есть запись и файл
        if not need_generate:
            for ch in ALL_CHARS:
                unicode_code = f"U+{ord(ch):04X}"
                filename = char_map.get(unicode_code)
                if not filename or not (chars_path / filename).exists():
                    need_generate = True
                    break
    
    if need_generate:
        print(f"[font_utils] Генерация символов из шрифта {font_path}...")
        generate_char_images(font_path, chars_dir, char_height)
        create_char_map(chars_dir, char_map_json)
        print(f"[font_utils] Символы готовы к использованию")

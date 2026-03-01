"""
Утилиты для работы с изображениями
"""
import io
from pathlib import Path
from typing import Tuple

from PIL import Image, ImageDraw
import base64


def generate_test_image(size_kb=4):
    """
    Генерирует тестовую JPG картинку заданного размера
    
    Args:
        size_kb: Целевой размер в килобайтах (по умолчанию 4 КБ)
    
    Returns:
        bytes: JPG данные картинки
    """
    # Начинаем с небольшого изображения и увеличиваем качество/размер
    width = 200
    height = 150
    
    # Создаем изображение с тестовым паттерном
    img = Image.new('RGB', (width, height), color='white')
    draw = ImageDraw.Draw(img)
    
    # Рисуем тестовый паттерн для визуальной проверки
    # Вертикальные полосы
    for x in range(0, width, 20):
        draw.rectangle([x, 0, x + 10, height], fill='blue' if (x // 20) % 2 == 0 else 'red')
    
    # Горизонтальные полосы
    for y in range(0, height, 15):
        draw.rectangle([0, y, width, y + 7], fill='green' if (y // 15) % 2 == 0 else 'yellow')
    
    # Текст для идентификации
    try:
        draw.text((10, 10), "TEST IMAGE", fill='black')
        draw.text((10, 30), f"Size: {size_kb}KB", fill='black')
    except:
        pass  # Если шрифт недоступен, пропускаем
    
    # Сохраняем в JPG с настройкой качества для достижения нужного размера
    output = io.BytesIO()
    quality = 95
    
    # Пробуем разные значения качества, чтобы достичь нужного размера
    for q in range(95, 10, -5):
        output.seek(0)
        output.truncate(0)
        img.save(output, format='JPEG', quality=q, optimize=True)
        
        size_bytes = len(output.getvalue())
        if size_bytes <= size_kb * 1024:
            break
    
    # Если все еще слишком большой, уменьшаем размер изображения
    while len(output.getvalue()) > size_kb * 1024 and (width > 50 or height > 50):
        width = int(width * 0.9)
        height = int(height * 0.9)
        img = img.resize((width, height), Image.Resampling.LANCZOS)
        output.seek(0)
        output.truncate(0)
        img.save(output, format='JPEG', quality=quality, optimize=True)
    
    return output.getvalue()


def mirror_image_horizontal(image_bytes):
    """
    Отзеркаливает изображение по горизонтали
    
    Args:
        image_bytes: JPG данные изображения
    
    Returns:
        bytes: JPG данные отзеркаленного изображения
    """
    # Загружаем изображение из байтов
    img = Image.open(io.BytesIO(image_bytes))
    
    # Отзеркаливаем по горизонтали
    mirrored = img.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
    
    # Сохраняем обратно в JPG
    output = io.BytesIO()
    mirrored.save(output, format='JPEG', quality=95, optimize=True)
    
    return output.getvalue()


def verify_image_integrity(original_bytes, received_bytes):
    """
    Проверяет целостность полученного изображения
    
    Args:
        original_bytes: Исходные JPG данные
        received_bytes: Полученные JPG данные
    
    Returns:
        tuple: (is_valid, error_message)
    """
    if not received_bytes:
        return False, "Получены пустые данные"
    
    if len(received_bytes) < 100:
        return False, f"Полученные данные слишком маленькие: {len(received_bytes)} байт"
    
    # Проверяем, что это валидный JPG
    try:
        img = Image.open(io.BytesIO(received_bytes))
        img.verify()
    except Exception as e:
        return False, f"Невалидный формат JPG: {str(e)}"
    
    # Проверяем размер (должен быть примерно таким же, может немного отличаться из-за сжатия)
    size_diff = abs(len(received_bytes) - len(original_bytes))
    size_diff_percent = (size_diff / len(original_bytes)) * 100
    
    if size_diff_percent > 50:  # Допускаем разницу до 50%
        return False, f"Размер сильно отличается: оригинал {len(original_bytes)} байт, получено {len(received_bytes)} байт"
    
    return True, "OK"


def display_image(image_bytes, title="Image"):
    """
    Отображает изображение (для консольного вывода информации)
    
    Args:
        image_bytes: JPG данные изображения
        title: Заголовок для отображения
    """
    try:
        img = Image.open(io.BytesIO(image_bytes))
        print(f"\n{title}:")
        print(f"  Размер: {img.size[0]}x{img.size[1]} пикселей")
        print(f"  Формат: {img.format}")
        print(f"  Размер файла: {len(image_bytes)} байт ({len(image_bytes)/1024:.2f} КБ)")
        
        # Пытаемся показать изображение (если есть GUI)
        # try:
        #     # img.show(title=title)
        # except:
        #     # print(f"  (Не удалось отобразить изображение - возможно, нет GUI)")
    except Exception as e:
        print(f"Ошибка при обработке изображения: {e}")


def image_to_base64(image_bytes):
    """
    Конвертирует изображение в base64 строку
    
    Args:
        image_bytes: JPG данные
    
    Returns:
        str: base64 строка
    """
    return base64.b64encode(image_bytes).decode('utf-8')


def base64_to_image(base64_str):
    """
    Конвертирует base64 строку в изображение
    
    Args:
        base64_str: base64 строка
    
    Returns:
        bytes: JPG данные
    """
    return base64.b64decode(base64_str)


def load_bg_image(path: str = "bg.jpg") -> Image.Image:
    """
    Загружает фоновое изображение и приводит его к размеру дисплея 480x320.

    Args:
        path: Относительный или абсолютный путь к файлу bg.jpg.

    Returns:
        PIL.Image в режиме RGB размером 480x320.
    """
    p = Path(path)
    if not p.is_file():
        # Пытаемся найти рядом с текущим модулем (pyapp/bg.jpg)
        module_dir = Path(__file__).resolve().parent
        candidate = module_dir / "bg.jpg"
        if candidate.is_file():
            p = candidate

    img = Image.open(p).convert("RGB")

    target_w, target_h = 480, 320

    # Масштабируем с сохранением пропорций, затем центрируем и обрезаем/дополняем
    img = _resize_and_letterbox(img, (target_w, target_h))
    return img


def _resize_and_letterbox(img: Image.Image, target_size: Tuple[int, int]) -> Image.Image:
    """
    Масштабирует изображение с сохранением пропорций и вписывает в целевой размер
    с возможным добавлением полей (letterbox) по краям.
    """
    target_w, target_h = target_size
    src_w, src_h = img.size

    scale = min(target_w / src_w, target_h / src_h)
    new_w = int(src_w * scale)
    new_h = int(src_h * scale)

    resized = img.resize((new_w, new_h), Image.Resampling.LANCZOS)

    canvas = Image.new("RGB", (target_w, target_h), color="black")
    offset_x = (target_w - new_w) // 2
    offset_y = (target_h - new_h) // 2
    canvas.paste(resized, (offset_x, offset_y))
    return canvas


def image_to_rgb565_be(img: Image.Image) -> bytes:
    """
    Конвертирует изображение в буфер RGB565 (big-endian HI,LO на пиксель),
    совместимый с контроллером ILI9486.

    Args:
        img: PIL.Image в любом режиме (будет приведено к RGB).

    Returns:
        bytes: последовательно упакованные пиксели в формате RGB565 BE.
    """
    if img.mode != "RGB":
        img = img.convert("RGB")

    width, height = img.size
    pixels = img.load()

    out = bytearray(width * height * 2)
    idx = 0

    for y in range(height):
        # ВАЖНО для ИИ-агента: Читаем пиксели справа налево для исправления зеркалирования
        # Без этого изображение будет отзеркалено по горизонтали на дисплее
        # Если изображение всё равно зеркалится, попробуйте убрать reversed() или изменить порядок
        # Порядок чтения: от width-1 до 0 включительно (справа налево)
        for x in reversed(range(width)):  # от width-1 до 0 включительно
            r, g, b = pixels[x, y]
            # Преобразование в RGB565 (5 бит красный, 6 бит зелёный, 5 бит синий)
            r5 = (r >> 3) & 0x1F  # 5 бит из 8
            g6 = (g >> 2) & 0x3F  # 6 бит из 8
            b5 = (b >> 3) & 0x1F  # 5 бит из 8

            value = (r5 << 11) | (g6 << 5) | b5  # Формат: RRRRR GGGGGG BBBBB
            # Big-endian формат для ILI9486: старший байт первым
            out[idx] = (value >> 8) & 0xFF  # HI (старший байт)
            out[idx + 1] = value & 0xFF    # LO (младший байт)
            idx += 2

    return bytes(out)

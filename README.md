# VKOROBKA - Распределенная система обработки данных

## 📋 Обзор проекта

VKOROBKA - это распределенная система, состоящая из двух основных компонентов:
- **win-x64** - Windows приложение-маршрутизатор
- **esp32s3** - ESP32-S3 микроконтроллер (основной MCU, управляющий периферией)

Система использует единый протокол обмена сообщениями (`message_protocol.h`) для коммуникации между компонентами и поддерживает обработку изображений, команд и потоковых данных.

---

## 🏗️ Архитектура системы

### Схема коммуникации (основная цепочка)

```
┌─────────────┐
│   Python    │  (pyapp/)
│   Client    │
└──────┬──────┘
       │ UDP (JSON)
       │ Port 1236
       ▼
┌─────────────────────────────────┐
│      win-x64 (Windows)          │
│  ┌───────────────────────────┐  │
│  │   message_router           │  │  JSON ↔ Binary
│  │   (маршрутизация)          │  │
│  └───────────────────────────┘  │
│  ┌───────────────────────────┐  │
│  │   image_processor         │  │  Обработка JPG
│  └───────────────────────────┘  │
└──────┬──────────────────────────┘
       │ TCP (Binary)
       │ Port 1234
       ▼
┌─────────────────────────────────┐
│      esp32s3 (ESP32-S3)         │
│  ┌───────────────────────────┐  │
│  │   message_bridge           │  │  TCP ↔ локальные обработчики
│  │   дисплей / аудио / др.    │  │  (периферия на ESP32-S3)
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

### Протокол обмена сообщениями

Все основные компоненты используют единый бинарный протокол (`message_protocol.h`):

**Заголовок сообщения (12 байт):**
- `msg_type` (1 байт) - тип сообщения (COMMAND, DATA, STREAM, RESPONSE, ERROR)
- `source_id` (1 байт) - ID источника (WIN, ESP32, EXTERNAL)
- `destination_id` (1 байт) - ID получателя
- `route_flags` (1 байт) - флаги маршрутизации
- `priority` (1 байт) - приоритет (0 = highest, 255 = lowest)
- `stream_id` (2 байта) - ID потока для потоковых данных
- `payload_len` (4 байта) - длина payload
- `sequence` (1 байт) - порядковый номер

**Формат передачи:**
- **Python ↔ win-x64**: JSON через UDP (порт 1236)
- **win-x64 ↔ ESP32**: Бинарный формат через TCP (порт 1234)


**Правило обработки ошибок:**
- Если получатель не может обработать/принять полностью заголовок сообщения - он сбрасывается без уведомления отправителю
- Получатель не пытается искать валидный заголовок, пропуская байты - невалидные данные просто отбрасываются

---

## 📦 Модули проекта

### 1. win-x64 (Windows приложение)

**Назначение:** Центральный маршрутизатор системы, обрабатывает запросы от Python клиента и маршрутизирует их к ESP32 или обрабатывает локально.

**Основные компоненты:**
- **message_router** - маршрутизация сообщений между компонентами
- **tcp** - TCP сервер для связи с ESP32
- **udp_api** - UDP сервер для связи с Python клиентом
- **wifi** - управление WiFi интерфейсом (для будущего использования)
- **image_processor** - обработка JPG изображений (отзеркаливание)

**Файлы:**

| Файл | Назначение |
|------|-----------|
| `main.cpp` | Точка входа, инициализация компонентов, обработка сообщений |
| `message_router.hpp/cpp` | Маршрутизация сообщений между TCP/UDP, конвертация JSON↔Binary |
| `tcp.hpp/cpp` | TCP сервер для связи с ESP32, обработка бинарных сообщений |
| `udp_api.hpp/cpp` | UDP сервер для связи с Python клиентом, обработка JSON |
| `wifi.hpp/cpp` | Управление WiFi интерфейсом (заготовка) |
| `image_processor.hpp/cpp` | Обработка JPG изображений (отзеркаливание через GDI+) |
| `cvar.hpp/cpp` | Система команд для консоли (help, send, status, test_esp32) |
| `sys.hpp` | Системные определения (ANSI цвета, типы) |

**Ключевые функции:**
- Прием JSON сообщений от Python через UDP
- Конвертация JSON → бинарный формат
- Маршрутизация к ESP32 через TCP или локальная обработка
- Обработка изображений (отзеркаливание JPG)
- Конвертация ответов обратно в JSON и отправка Python клиенту

**Для ИИ-агента:**
- 🔍 **Ошибки маршрутизации**: проверьте `message_router.cpp` - функции `route_from_buffer()`, `convert_json_to_binary()`
- 🔍 **Проблемы с TCP**: проверьте `tcp.cpp` - функции `start()`, `handle_connection()`
- 🔍 **Проблемы с UDP**: проверьте `udp_api.cpp` - функции `start()`, `handle_receive()`
- 🔍 **Ошибки обработки изображений**: проверьте `image_processor.cpp` - функция `mirror_jpeg_horizontal()`
- 🔍 **Проблемы с JSON**: проверьте `message_router.cpp` - функции `parse_json_message()`, `create_json_message()`

---

### 2. esp32s3 (ESP32-S3 микроконтроллер)

**Назначение:** Основной MCU системы. Получает команды и данные от win-x64 по TCP, управляет периферией (дисплей, микрофон, динамик и др.) и возвращает результаты обратно в цепочку pyapp ↔ win-x64 ↔ esp32s3.

**Основные компоненты:**
- **wifi** - подключение к WiFi сети
- **tcp** - TCP клиент для связи с win-x64
- **message_bridge** - маршрутизация сообщений и вызов локальных обработчиков (ESP32-S3)
- **ili9486_display / i80_lcd_bus / display_pins** - работа с TFT-дисплеем по аппаратной I80-шине

Дополнительные компоненты (текстинг и потоки):

- **texting** (`texting.hpp/cpp`) - обработка команд `TEXT_CLEAR` / `TEXT_ADD` и по-символьный вывод текста на дисплей.
- **stream_handler** (`stream_handler.hpp/cpp`) - логика потокового вывода RGB565-кадров (фоновые изображения и другие сценарии).
- **command_handler** (`command_handler.hpp/cpp`) - обработка высокоуровневых команд TEST / STATUS / TEXTING / IMAGE_DATA.
- **message_dispatcher** (`message_dispatcher.hpp/cpp`) - единая точка входа для сообщений из `message_bridge`, распределяет их в `command_handler` и `stream_handler`.

**Файлы:**

| Файл | Назначение |
|------|-----------|
| `main.cpp` | Точка входа, инициализация WiFi/TCP, обработка команд TEST/STATUS/DATA для ESP32-S3 |
| `message_bridge.hpp/cpp` | Маршрутизация сообщений между TCP и локальными обработчиками, обработка протокола |
| `tcp.hpp/cpp` | TCP клиент для связи с win-x64, автоматическое переподключение |
| `wifi.hpp/cpp` | WiFi клиент, подключение к сети, управление состоянием |

**Ключевые функции:**
- Подключение к WiFi сети (настройки в `main.cpp`: WIFI_SSID, WIFI_PASS)
- Подключение к win-x64 через TCP (настройки: SERVER_IP, SERVER_PORT)
- Маршрутизация сообщений TCP ↔ локальные обработчики
- Обработка команд TEST и STATUS от win-x64
- **Потоковый вывод изображений на ILI9486 дисплей** (MAR3501, 480x320)
  - Приём чанков RGB565 по строкам через STREAM сообщения
  - Буферизация неупорядоченных чанков (до 32 чанков)
  - Потоковый вывод на дисплей без накопления всего кадра в памяти
  - Отправка подтверждений (CHUNK_ACK) для каждого обработанного чанка
  - Финальный ответ LCD_FRAME_OK после завершения всех чанков
- **Текстинг на дисплей (реалистичная печать текста)**:
  - ESP32 получает команду `TEXT_ADD` от pyapp (источник `MSG_SRC_EXTERNAL`, тип `MSG_TYPE_COMMAND`).
  - В `command_handler.cpp` (`command_handle_texting`) по полю `"command"` отличает `TEXT_CLEAR` и `TEXT_ADD` и вызывает `texting_handle_clear()` / `texting_handle_add()`.
  - В `texting_handle_clear()`:
    - из JSON берутся `field_x`, `field_y`, `field_width`, `field_height`;
    - прямоугольник заливается белым (`fillRect(..., 0xFFFF)`).
  - В `texting_handle_add()`:
    - парсятся параметры поля, высота строки и скорость печати;
    - парсится строка `text` (UTF-8);
    - из секции `"chars"` загружаются и декодируются глифы:
      - для каждого `"U+XXXX"` читаются `width`, `height` и base64-данные RGB565;
      - данные кладутся в `TextingState::chars[]` (структура `CharData`).
    - запускается FreeRTOS-задача `texting_render_task`.
  - `texting_render_task`:
    - разбирает `text` в Unicode-символы (поддержка ASCII, 2- и 3-байтовых UTF-8, включая кириллицу);
    - реализует перенос по словам:
      - слово (последовательность непробельных символов) не рвётся между строками, если целиком помещается в строку;
      - слишком длинные слова (шире поля) допускается переносить посимвольно.
    - использует общую высоту строки (`char_height + line_spacing`) и фиксированный line box:
      - все глифы уже содержат общую базовую линию (заложено на стороне `pyapp` в PNG);
      - символы выводятся по одной `y`-координате строки (натуральный вид текста).
    - реализует переполнение по вертикали:
      - при достижении `max_lines` поле прокручивается: первая строка очищается и текст продолжается с начала.
    - имитирует «печать» по одному символу:
      - между символами используется `vTaskDelay(typing_speed_ms)`.

**Для ИИ-агента:**
- 🔍 **Проблемы с WiFi**: проверьте `wifi.cpp` - функции `init()`, `wait_for_connection()`
- 🔍 **Проблемы с TCP**: проверьте `tcp.cpp` - функции `start()`, `tcp_task()`, состояние `TCP_STATE_CONNECTING`
- 🔍 **Проблемы с UART**: проверьте `uart_bridge.cpp` - функции `init()`, `uart_rx_task()`, настройки пинов (TX=17, RX=16)
- 🔍 **Ошибки маршрутизации**: проверьте `message_bridge.cpp` - функции `route_from_tcp()`, `route_from_uart()`, `process_buffer()`
- 🔍 **Проблемы с протоколом**: проверьте `message_bridge.cpp` - функция `process_buffer()` использует `msg_unpack()` из `message_protocol.h`
- 🔍 **Потоковый вывод изображений**: проверьте `main.cpp` - обработка `MSG_TYPE_STREAM` с `STREAM_ID_LCD_FRAME`, структура `LcdStreamState`, функция `drawRgb565Chunk()` в `ili9486_display.cpp`
- 🔍 **Проблемы с памятью**: ESP32 не накапливает весь кадр (307KB), а выводит чанки сразу. Если нужно больше буфера для неупорядоченных чанков, увеличьте `MAX_BUFFERED_CHUNKS` в `LcdStreamState`

**Важные настройки в main.cpp:**
```cpp
#define WIFI_SSID "ModelSim"        // Имя WiFi сети
#define WIFI_PASS "adminadmin"      // Пароль WiFi
#define SERVER_IP   "192.168.43.236" // IP адрес win-x64
#define SERVER_PORT 1234             // TCP порт
```

---

### 3. pyapp (Python тестовый клиент)

**Назначение:** Тестовое приложение для проверки работы основного пути pyapp ↔ win-x64 ↔ esp32s3.

**Файлы:**

| Файл | Назначение |
|------|-----------|
| `main.py` | Основной скрипт, последовательное тестирование всех компонентов |
| `vkorobka_client.py` | UDP клиент для связи с win-x64, отправка/прием JSON сообщений, потоковая передача изображений и команд |
| `image_utils.py` | Утилиты для работы с изображениями (генерация, отображение, проверка, конвертация в RGB565) |
| `show_bg.py` | Скрипт для отображения фонового изображения (bg.jpg) на ESP32 дисплее (потоковая передача) |
| `font_utils.py` | Генерация и кэширование PNG-глифов из TTF/OTF шрифта, маппинг Unicode → файл, загрузка кэша символов |
| `texting.py` | Логика текстинга: управление текстовым полем, генерация payload `TEXT_CLEAR` / `TEXT_ADD` для ESP32 |
| `texting_cli.py` | CLI для управления текстингом (очистка поля, добавление текста, регенерация шрифта) |

**Ключевые функции:**
- Генерация тестовых JPG изображений
- Отправка изображений на компоненты (win, esp32)
- **Потоковая передача изображений на дисплей ESP32** (с подтверждениями чанков)
- Получение и проверка ответов
- Визуализация результатов
- Управление текстингом на дисплее (очистка поля, добавление текста, регенерация шрифта и карты символов)

#### Текстинг (реалистичная печать текста)

Текстинг реализован поверх общего протокола команд:

- `TEXT_CLEAR` — очистка прямоугольного текстового поля (заливка белым).
- `TEXT_ADD` — добавление текста с эффектом по-символьной печати.

`texting.py`:

- Управляет параметрами поля: `field_x=52`, `field_y=160`, `field_width=380`, `field_height=120`.
- Управляет типографикой: `char_height`, `line_spacing`, `typing_speed_ms`.
- С помощью `font_utils.py` генерирует и кэширует PNG-глифы:
  - файлы именуются по Unicode-коду: `U0041.png` для `A`, `U0430.png` для `а` и т.п.;
  - все глифы рисуются в общий line box (ascent+descent), c единой базовой линией (как в обычном шрифтовом рендерере).
- Формирует JSON-payload для ESP32:
  - `text` — строка UTF-8;
  - `field_x`, `field_y`, `field_width`, `field_height`;
  - `char_height`, `line_spacing`, `typing_speed_ms`;
  - `cursor_x`, `cursor_y`, `current_line_index`;
  - `chars` — словарь `{ "U+XXXX": { "width", "height", "data" } }`, где `data` — base64 от RGB565 big-endian.

`texting_cli.py`:

- Команда `clear` — отправка `TEXT_CLEAR`:
  ```bash
  python texting_cli.py --command clear
  ```
- Команда `add_text` — отправка `TEXT_ADD`:
  ```bash
  python texting_cli.py --command add_text --text "Привет, мир!" \
    --char-height 16 --typing-speed 100
  ```
- Флаг `--regen-font` — полная регенерация PNG-глифов и карты символов из TTF/OTF:
  ```bash
  python texting_cli.py --command add_text --text "Hello World!" \
    --char-height 16 --typing-speed 100 \
    --regen-font
  ```
  При этом:
  - пересоздаётся директория `fonts/chars` с файлами `UXXXX.png` для всех символов `ALL_CHARS`;
  - пересоздаётся `fonts/char_map.json` с маппингом `Unicode → файл`.

**Потоковая передача изображений на дисплей:**
- Изображение разбивается на чанки по строкам (по умолчанию 20 строк на чанк)
- Каждый чанк отправляется как STREAM сообщение с внутренним заголовком
- Python ждёт подтверждения (CHUNK_ACK) перед отправкой следующего чанка (flow control)
- ESP32 выводит чанки на дисплей по мере поступления (без накопления всего кадра в памяти)
- После завершения всех чанков ESP32 отправляет финальный ответ LCD_FRAME_OK

**Для ИИ-агента:**
- 🔍 **Проблемы с UDP**: проверьте `vkorobka_client.py` - функции `send_test_message()`, `_receive_loop()`
- 🔍 **Ошибки JSON**: проверьте `vkorobka_client.py` - функции `send_test_message()`, `_handle_response()`
- 🔍 **Проблемы с изображениями**: проверьте `image_utils.py` - функции `generate_test_image()`, `base64_to_image()`
- 🔍 **Потоковая передача**: проверьте `vkorobka_client.py` - функция `send_lcd_frame_rgb565()`, обработка CHUNK_ACK в `_handle_response()`
- 🔍 **Таймауты**: увеличьте `timeout` в конструкторе `VkorobkaClient` (по умолчанию 10 секунд) или `chunk_ack_timeout` в `send_lcd_frame_rgb565()` (по умолчанию 5 секунд на чанк)

---

## 🔧 Общие компоненты

### message_protocol.h

**Расположение:** `.common/include/message_protocol.h` (используется всеми модулями)

**Назначение:** Единый протокол обмена сообщениями между всеми компонентами.

**Ключевые функции:**
- `msg_pack()` - упаковка сообщения в бинарный формат
- `msg_unpack()` - распаковка сообщения из бинарного формата
- `msg_create_header()` - создание заголовка сообщения
- `msg_validate_header()` - проверка валидности заголовка

**Для ИИ-агента:**
- 🔍 **Ошибки упаковки/распаковки**: проверьте порядок байт (little-endian), размеры полей
- 🔍 **Проблемы с заголовком**: убедитесь, что `MSG_HEADER_LEN = 12` байт
- 🔍 **Ошибки валидации**: проверьте `msg_validate_header()` - корректность типов, source_id, destination_id

---

## 🐛 Отладка и поиск ошибок

### Типичные проблемы и где их искать

#### 1. Сообщения не доходят от Python к ESP32

**Проверьте:**
1. `pyapp/vkorobka_client.py` - отправка UDP сообщений
2. `win-x64/udp_api.cpp` - прием UDP и конвертация в бинарный формат
3. `win-x64/message_router.cpp` - маршрутизация к ESP32
4. `win-x64/tcp.cpp` - отправка через TCP
5. `esp32s3/main/tcp.cpp` - прием TCP сообщений
6. `esp32s3/main/message_bridge.cpp` - маршрутизация к UART

**Логи для проверки:**
- Python: `[client] [TX]` - отправка сообщений
- win-x64: `[udp]`, `[tcp]`, `[app]` - обработка сообщений
- ESP32: `[RX]`, `[TX]` - прием/отправка сообщений

#### 2. TCP соединение не устанавливается

**Проверьте:**
1. `esp32s3/main/wifi.cpp` - подключение к WiFi
2. `esp32s3/main/tcp.cpp` - состояние TCP (`TCP_STATE_CONNECTING`, `TCP_STATE_CONNECTED`)
3. `win-x64/tcp.cpp` - TCP сервер запущен и слушает порт 1234
4. Настройки IP адреса в `esp32s3/main/main.cpp` (SERVER_IP)

#### 3. Ошибки обработки изображений

**Проверьте:**
1. `win-x64/image_processor.cpp` - функция `mirror_jpeg_horizontal()`, использование GDI+
2. `pyapp/image_utils.py` - функции `image_to_base64()`, `base64_to_image()`
3. Размеры изображений (не превышают `MSG_MAX_PAYLOAD_SIZE = 65535` байт)

#### 5. JSON конвертация не работает

**Проверьте:**
1. `win-x64/message_router.cpp` - функции `convert_json_to_binary()`, `convert_binary_to_json()`
2. `win-x64/message_router.cpp` - функция `parse_json_message()` для извлечения test_id
3. Формат JSON: поля `type`, `source`, `destination`, `payload` (base64), `test_id`

---

## 📝 Заметки для ИИ-агента

### Быстрая навигация по коду

**При поиске ошибок маршрутизации:**
- Начните с `win-x64/message_router.cpp` - центральный узел маршрутизации
- Проверьте `esp32s3/main/message_bridge.cpp` - мост между TCP и UART

**При проблемах с протоколом:**
- Проверьте `.common/include/message_protocol.h` - определения протокола
- Проверьте `.common/src/message_protocol.c` - реализация упаковки/распаковки
- Убедитесь, что все модули используют одну и ту же версию протокола

**При проблемах с сетью:**
- TCP: `win-x64/tcp.cpp` (сервер) и `esp32s3/main/tcp.cpp` (клиент)
- UDP: `win-x64/udp_api.cpp` (сервер) и `pyapp/vkorobka_client.py` (клиент)
- WiFi: `esp32s3/main/wifi.cpp`

**При проблемах с периферией:**
- UART ESP32: `esp32s3/main/uart_bridge.cpp`

### Важные константы и настройки

**Порты:**
- UDP (Python ↔ win-x64): 1236
- TCP (win-x64 ↔ ESP32): 1234

**UART настройки (ESP32):**
- Скорость: 115200 baud
- ESP32 пины: TX=17, RX=16 (UART_NUM_2)

**Размеры буферов:**
- `MSG_MAX_PAYLOAD_SIZE = 65535` байт
- `MSG_HEADER_LEN = 12` байт
- UART буферы: RX=4096, TX=2048 байт (ESP32)

### Типичные места ошибок

1. **Порядок байт (endianness)**: Все числовые поля в протоколе - little-endian
2. **Размеры буферов**: Проверяйте, что буферы достаточного размера для payload
3. **Синхронизация**: UDP и TCP работают асинхронно, используйте test_id для сопоставления запросов/ответов
4. **Очистка памяти**: В C++ используйте `std::vector`, в C - проверяйте `malloc/free`
5. **Таймауты**: Python клиент использует таймаут 10 секунд по умолчанию

### Рекомендуемый порядок отладки

1. **Проверьте логи** - начните с последнего успешного этапа коммуникации
2. **Проверьте протокол** - убедитесь, что сообщения правильно упакованы/распакованы
3. **Проверьте сеть** - ping, telnet для проверки соединений
4. **Проверьте периферию** - осциллограф/логический анализатор для UART
5. **Проверьте настройки** - IP адреса, порты, скорости передачи

---

## 🚀 Сборка и запуск

### win-x64

```bash
cd win-x64
# Откройте vkorobka.slnx в Visual Studio
# Соберите проект (F7)
# Запустите (F5)
```

**Требования:**
- Visual Studio 2022 или новее
- Windows SDK
- GDI+ (входит в Windows)

### esp32s3

```bash
cd esp32s3
idf.py build
idf.py flash monitor
```

**Требования:**
- ESP-IDF v5.0 или новее
- Python 3.8+
- Настроенный WiFi (измените WIFI_SSID и WIFI_PASS в main.cpp)

### pyapp

```bash
cd pyapp
pip install -r requirements.txt
python main.py
```

**Требования:**
- Python 3.8+
- Pillow
- Запущенное win-x64 приложение

---

## 📚 Дополнительная информация

### Протоколы

**Новый message_protocol** (используется везде):
- Заголовок: 12 байт (см. `message_protocol.h`)
- Типы сообщений: COMMAND, DATA, STREAM, RESPONSE, ERROR
- Поддержка приоритетов, потоков, sequence номеров

### Тестирование

Python клиент (`pyapp/main.py`) автоматически тестирует все компоненты:
1. Генерирует тестовое JPG изображение (4 КБ)
2. Отправляет на компоненты (win, esp32)
3. Проверяет, что получен отзеркаленный ответ
4. Выводит результаты тестирования

---

## 📄 Лицензия

Проект использует различные лицензии для разных компонентов (см. соответствующие файлы исходного кода).

---

## 👥 Контакты и поддержка

При возникновении проблем:
1. Проверьте логи всех компонентов
2. Убедитесь, что все настройки (IP, порты, WiFi) корректны
3. Проверьте физические соединения (UART, при необходимости)
4. Используйте раздел "Отладка и поиск ошибок" выше

---

*Последнее обновление: 2024*

## 🖼️ Потоковая передача изображений на дисплей ESP32

### Обзор функциональности

Система поддерживает потоковую передачу изображений на дисплей ESP32 (ILI9486, MAR3501, 480x320) с подтверждениями для предотвращения переполнения буферов.

**Цепочка передачи:**
```
pyapp (show_bg.py) → win-x64 → esp32s3 → ILI9486 дисплей
```

**Процесс:**
1. Python загружает изображение (`bg.jpg`), приводит к 480x320, конвертирует в RGB565 big-endian
2. Изображение разбивается на чанки по строкам (по умолчанию 20 строк = ~19KB на чанк)
3. Каждый чанк отправляется как STREAM сообщение с внутренним заголовком:
   - `u16 width, height, chunk_index, total_chunks, format_code, y_start, height_chunk`
   - Затем данные чанка (RGB565, height_chunk строк)
4. Python ждёт подтверждения (CHUNK_ACK) перед отправкой следующего чанка (flow control)
5. ESP32 получает чанки, буферизует неупорядоченные, выводит на дисплей по мере готовности
6. После завершения всех чанков ESP32 отправляет финальный ответ LCD_FRAME_OK

**Важные детали реализации:**
- **Flow control**: Python не отправляет следующий чанк без подтверждения предыдущего
- **Буферизация**: ESP32 может буферизовать до 32 неупорядоченных чанков
- **Память**: ESP32 не накапливает весь кадр (307KB), а выводит чанки сразу на дисплей
- **test_id**: win-x64 сохраняет test_id для всех CHUNK_ACK и очищает только для финального LCD_FRAME_OK
- **Зеркалирование**: Исправлено чтением пикселей справа налево в `image_to_rgb565_be()`

**Использование:**
```bash
cd pyapp
python show_bg.py
```

**Файлы:**
- `pyapp/show_bg.py` - основной скрипт
- `pyapp/vkorobka_client.py` - метод `send_lcd_frame_rgb565()`, обработка CHUNK_ACK
- `pyapp/image_utils.py` - `load_bg_image()`, `image_to_rgb565_be()`
- `esp32s3/main/main.cpp` - обработка STREAM сообщений, структура `LcdStreamState`
- `esp32s3/main/ili9486_display.cpp` - метод `drawRgb565Chunk()`

**Для ИИ-агента:**
- 🔍 **Проблемы с подтверждениями**: проверьте, что win-x64 не очищает test_id для CHUNK_ACK (только для LCD_FRAME_OK)
- 🔍 **Неполный вывод изображения**: проверьте логику завершения кадра в `main.cpp` - все три условия должны быть выполнены (received_chunks >= total_chunks, next_y >= height, buffered_count == 0)
- 🔍 **Зеркалирование**: если изображение отзеркалено, проверьте порядок чтения пикселей в `image_to_rgb565_be()` - должно быть `reversed(range(width))`
- 🔍 **Таймауты**: если чанки не приходят, увеличьте `chunk_ack_timeout` в `send_lcd_frame_rgb565()` или проверьте скорость вывода на дисплей

---

## ✏️ Реалистичный текстинг на дисплей ESP32

### Обзор

Текстинг реализует вывод текста в прямоугольное поле на дисплее ILI9486 (MAR3501) с эффектом печати по символам, как в современных чатах.

**Цепочка:**

```
pyapp (texting_cli.py / texting.py) → win-x64 → esp32s3 (texting.cpp) → ILI9486 дисплей
```

### Параметры текстового поля

По умолчанию:

- Начало поля: `(52, 160)`
- Размер поля: `380x120` пикселей
- Высота строки (line box): задаётся `char_height` и `line_spacing`
- Скорость печати: `typing_speed_ms` (например, 50–100 мс на символ)

### Поведение на ESP32

- Текст разбирается в Unicode-символы (UTF-8), поддерживаются кириллица, латиница, цифры и спецсимволы.
- Перенос по словам:
  - если слово не помещается в остаток строки — оно целиком переносится на следующую строку;
  - если слово шире всей строки — допускается перенос внутри слова.
- Все глифы рисуются в общем line box с единой базовой линией:
  - буквы с нижними/верхними выносными элементами (Ц, Щ, у, р, б) выглядят «натурально»;
  - цифры и спецзнаки выровнены так же, как в TTF/OTF-шрифте.
- При переполнении поля:
  - текст начинает заново с первой строки, предварительно очищая строки по мере продвижения курсора.
- Эффект печати:
  - каждый символ выводится с задержкой `typing_speed_ms`.

### Использование из pyapp

- Очистка поля:

```bash
cd pyapp
python texting_cli.py --command clear
```

- Печать текста:

```bash
python texting_cli.py --command add_text --text "Привет, мир! Hello, World!" \
  --char-height 16 --line-spacing 2 --typing-speed 100
```

- Полная регенерация шрифта и карты символов (например, после смены TTF):

```bash
python texting_cli.py --command add_text --text "Тест текстинга" \
  --char-height 16 --typing-speed 80 \
  --regen-font
```

При этом будут сгенерированы:

- PNG-глифы `UXXXX.png` для всех символов `ALL_CHARS` в `fonts/chars/`;
- `fonts/char_map.json` с маппингом `Unicode → имя файла`.

---

## ESP32-S3 + ILI9486 (MAR3501) – I80 LCD Demo

### Overview

This project drives a `MAR3501` LCD module (ILI9486 controller, 480x320, 8‑bit parallel bus)
from an ESP32‑S3 using the ESP‑IDF `esp_lcd` I80 (8080) interface.

- **Only hardware I80 path is used** – no GPIO bit‑banging in the main code path.
- The display driver is encapsulated in `Ili9486Display`:
  - `init()` – reset + ILI9486 init sequence via I80
  - `fillScreen(color)` – fill entire screen with a solid RGB565 color
  - `fillRect(x, y, w, h, color)` – fill a rectangle in pixels
  - `drawTestPattern()` – render color / grayscale gradients for visual testing
- On boot, `app_main()` initializes I80 + ILI9486 and calls `drawTestPattern()` once
  to show a static test image. Wi‑Fi/TCP code is present but disabled.

### Pins (ESP32‑S3 ↔ MAR3501)

8‑bit data bus plus control lines:

| ESP32‑S3 GPIO | MAR pin |
|---------------|---------|
| 4             | LCD_D0  |
| 5             | LCD_D1  |
| 6             | LCD_D2  |
| 7             | LCD_D3  |
| 8             | LCD_D4  |
| 9             | LCD_D5  |
| 10            | LCD_D6  |
| 11            | LCD_D7  |
| 12            | LCD_RS (D/C) |
| 13            | LCD_CS  |
| 14            | LCD_RST |
| 15            | LCD_WR  |
| 16            | LCD_RD  |

### How to build and run

1. Install ESP‑IDF (tested with v5.5.2).
2. From `esp32s3/` run:
   - `idf.py set-target esp32s3`
   - `idf.py -p COMx flash monitor` (replace `COMx` with your port).
3. On boot you should see a static test image:
   - top 3/4 of the screen – smooth RGB gradient (R: left→right, G: top→bottom, B: right→left),
   - bottom area – horizontal grayscale ramp (black→white).

### Notes

- `LCD_DEBUG_NO_NET` is defined in `main.cpp`, so Wi‑Fi/TCP initialization is currently **disabled**.
  You can later remove or comment this macro to enable the existing Wi‑Fi/TCP code.
- The ILI9486 is configured for **RGB565** (`0x3A = 0x55`) and display orientation (`0x36 = 0xE8`)
  to match a landscape layout: `(0,0)` is the top‑left corner, `(479,319)` is bottom‑right.


## ESP32-S3 + INMP441 (draft readme section)

INMP441 имеет высокое качество звука, низкий уровень шума и легко подключается к ESP32. Поскольку это цифровой микрофон, аналоговые помехи, характерные для традиционных микрофонов, здесь отсутствуют.

Характеристики INMP441:

Рабочее напряжение: 1,8–3,3 В
Потребление: 1,4 мА
Отношение сигнал/шум: 61 дБА
Частота дискретизации: до 48 кГц
Формат выходных данных: 24-бит I²С-compatible (режим I²S)

К микроконтроллеру ESP32-S3 подключён модуль микрофона INMP441; управление по I2S (монозвук, L/R -> GND => Left channel).

### Pins (ESP32‑S3 ↔ INMP441)

| ESP32‑S3 GPIO | MIC pin  |
|---------------|----------|
| 35            | MIC_SD   |
| 36            | MIC_WS   |
| 37            | MIC_SCK  |

## ESP32-S3 + MAX98357A (draft readme section)

The MAX98357A/MAX98357B are digital PCM input
Class D power amplifiers. The MAX98357A accepts standard I2S data through DIN, BCLK, and LRCLK while the
MAX98357B accepts left-justified data through the same
inputs. Both versions also accept 16-bit or 32-bit TDM
data with up to eight slots. The digital audio interface
eliminates the need for an external MCLK signal that is
typically required for I2S data transmission.
SD_MODE selects which data word is output by the
amplifier and is used to put the ICs into shutdown. These
devices offer five gain settings in I2S/left-justified mode
and a fixed 12dB gain in TDM mode. Channel selection in
TDM mode is set with the combination of SD_MODE and
GAIN_SLOT (Table 7).
The MAX98357A/MAX98357B DAI includes a DC blocker
with a -3dB cutoff at 3.7Hz.
The MAX98357A/MAX98357B feature low-quiescent current, comprehensive click-and-pop suppression, and
excellent RF immunity. The ICs offer Class AB audio
performance with Class D efficiency in a minimal boardspace solution. The Class D amplifier features spreadspectrum modulation with edge-rate and overshoot control circuitry that offers significant improvements in switchmode amplifier radiated emissions. The amplifier features
click-and-pop suppression that reduces audible transients
on startup and shutdown. The amplifier includes thermaloverload and short-circuit protection.
Digital Audio Interface Modes
The input stage of the digital audio interface is highly flexible, supporting 8kHz–96kHz sampling rates with 16/24/32-
bit resolution for I2S/left justified data as well as up to a
8-slot, 16-bit or 32-bit time division multiplexed (TDM)
format. When LRCLK has a 50% duty cycle the data
format is determined by the part number selection
(MAX98357A/MAX98357B). When a frame sync pulse
is used for the LRCLK the data format is automatically
configured in TDM mode. The frame sync pulse indicates
the beginning of the first time slot.
MCLK Elimination
The ICs eliminate the need for the external MCLK
signal that is typically used for PCM communication.
This reduces EMI and possible board coupling issues in
addition to reducing the size and pin-count of the ICs.
BCLK Jitter Tolerance
The ICs feature a BCLK jitter tolerance of 0.5ns for RMS
jitter below 40kHz and 12ns for wideband RMS jitter while
maintaining a dynamic range greater than 98dB (Table 1).
BCLK Polarity
When operating in I2S/left-justified mode, incoming serial
data is always clocked-in on the rising edge of BCLK.
In TDM mode, the MAX98357A clocks-in serial data on
the rising edge of BCLK while the MAX98357B clocks in
serial data on the falling edge of BCLK (Table 2).
LRCLK Polarity
LRCLK specifies whether left-channel data or rightchannel data is currently being read by the digital audio
interface. The MAX98357A indicates the left channel
word when LRCLK is low, and the MAX98357B indicates
the left channel word when LRCLK is high (Table 3).
LRCLK ONLY supports 8kHz, 16kHz, 32kHz, 44.1kHz,
48kHz, 88.2kHz and 96kHz frequencies. LRCLK clocks
at 11.025kHz, 12kHz, 22.05kHz and 24kHz are NOT supported. Do not remove LRCLK while BCLK is present.
Removing LRCLK while BCLK is present can cause unexpected output behavior including a large DC output voltage.
Standby Mode
The ICs automatically enter standby mode when BCLK
is removed. If BCLK stops toggling, the ICs automatically

FUNCTIONAL PIN DESCRIPTION

-------+-----------+---------------------------------------------------------------------------------------------
 TQFN  | NAME      | FUNCTION                                                                                    
-------+-----------+---------------------------------------------------------------------------------------------
 4     | SD_MODE   | Shutdown and Channel Select. Pull SD_MODE low to place the device in shutdown.             
       |           | In I2S or LJ mode, SD_MODE selects the data channel (Table 5).                              
       |           | In TDM mode, SD_MODE and GAIN_SLOT are both used for channel selection (Table 7).          
-------+-----------+---------------------------------------------------------------------------------------------
 1     | DIN       | Digital Input Signal                                                                        
-------+-----------+---------------------------------------------------------------------------------------------
 2     | GAIN_SLOT | Gain and Channel Selection. In I2S and LJ mode determines amplifier output gain (Table 8).
       |           | In TDM mode, used for channel selection with SD_MODE (Table 7).                            
       |           | In TDM mode, gain is fixed at 12 dB.                                                        
-------+-----------+---------------------------------------------------------------------------------------------
 16    | BCLK      | Bit Clock Input                                                                             
-------+-----------+---------------------------------------------------------------------------------------------
 14    | LRCLK     | Frame Clock. Left/right clock for I2S and LJ mode.                                          
       |           | Sync clock for TDM mode.                                                                    
-------+-----------+---------------------------------------------------------------------------------------------

### Pins (ESP32‑S3 ↔ MAX98357A)

| ESP32‑S3 GPIO | DYN pin      |
|---------------|--------------|
| 39            | DYN_LRCK     |
| 40            | DYN_BCLK     |
| 41            | DYN_DIN      |
| 42            | DYN_SD_MODE  |
| 3.3V          | DYN_GAIN_SL  |

Потоковое воспроизведение по TCP (протокол как у uplink микрофона, см. `mic_stream.cpp`):

- Команды `MSG_TYPE_COMMAND`, источник `EXTERNAL`, назначение `ESP32`, текстовый payload: **`dyn.on`** — включить приём PCM и вывести `SD_MODE` из shutdown; **`dyn.off`** — выключить, дренировать очередь, короткая тишина на I2S. Ответы: `DYN_ON_OK` / `DYN_OFF_OK`.
- Поток: **`MSG_TYPE_STREAM`**, `stream_id = 3`, заголовок 8 байт (`sample_rate_hz` u32 = **22050**, `sample_count` u16, `bits_per_sample` u16 = **16**), далее моно PCM **16-bit little-endian** (2 байта на сэмпл), до **512** сэмплов на пакет (~23 ms аудио на чанк). Пока не отправлен `dyn.on`, пакеты игнорируются. (Uplink микрофона по-прежнему 48 kHz / 24-bit, `stream_id = 2`.) У MAX98357 в даташите **22.05 kHz** не указан как поддерживаемый LRCLK — при артефактах можно сменить в прошивке на **16 kHz** или **32 kHz**.
- В прошивке ESP32-S3 микрофон (RX) привязан к **I2S0**, усилитель (TX) — к **I2S1**, чтобы не делить один контроллер (иначе возможны треск и предупреждение `i2s controller 0 has been occupied`).
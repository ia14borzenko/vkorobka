# VKOROBKA - Распределенная система обработки данных

## 📋 Обзор проекта

VKOROBKA - это распределенная система, состоящая из трех основных компонентов:
- **win-x64** - Windows приложение-маршрутизатор
- **esp32s3** - ESP32 микроконтроллер (WiFi-мост)
- **stm32f4_cubemx_example** - STM32 микроконтроллер (периферийное устройство)

Система использует единый протокол обмена сообщениями (`message_protocol.h`) для коммуникации между компонентами и поддерживает обработку изображений, команд и потоковых данных.

---

## 🏗️ Архитектура системы

### Схема коммуникации

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
│      esp32s3 (ESP32)            │
│  ┌───────────────────────────┐  │
│  │   message_bridge           │  │  TCP ↔ UART
│  │   (мост между TCP и UART)  │  │
│  └───────────────────────────┘  │
└──────┬──────────────────────────┘
       │ UART (Binary)
       │ 115200 baud
       ▼
┌─────────────────────────────────┐
│  stm32f4_cubemx_example (STM32) │
│  ┌───────────────────────────┐  │
│  │   uart_handler             │  │  Обработка UART
│  │   message_queue            │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

### Протокол обмена сообщениями

Все компоненты используют единый бинарный протокол (`message_protocol.h`):

**Заголовок сообщения (12 байт):**
- `msg_type` (1 байт) - тип сообщения (COMMAND, DATA, STREAM, RESPONSE, ERROR)
- `source_id` (1 байт) - ID источника (WIN, ESP32, STM32, EXTERNAL)
- `destination_id` (1 байт) - ID получателя
- `route_flags` (1 байт) - флаги маршрутизации
- `priority` (1 байт) - приоритет (0 = highest, 255 = lowest)
- `stream_id` (2 байта) - ID потока для потоковых данных
- `payload_len` (4 байта) - длина payload
- `sequence` (1 байт) - порядковый номер

**Формат передачи:**
- **Python ↔ win-x64**: JSON через UDP (порт 1236)
- **win-x64 ↔ ESP32**: Бинарный формат через TCP (порт 1234)
- **ESP32 ↔ STM32**: Бинарный формат через UART (115200 baud)

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

### 2. esp32s3 (ESP32 микроконтроллер)

**Назначение:** WiFi-мост между Windows приложением и STM32, маршрутизирует сообщения между TCP и UART.

**Основные компоненты:**
- **wifi** - подключение к WiFi сети
- **tcp** - TCP клиент для связи с win-x64
- **uart_bridge** - мост для связи со STM32 через UART
- **message_bridge** - маршрутизация сообщений между TCP и UART

**Файлы:**

| Файл | Назначение |
|------|-----------|
| `main.cpp` | Точка входа, инициализация WiFi/TCP/UART, обработка команд TEST/STATUS |
| `message_bridge.hpp/cpp` | Маршрутизация сообщений между TCP и UART, обработка протокола |
| `tcp.hpp/cpp` | TCP клиент для связи с win-x64, автоматическое переподключение |
| `uart_bridge.hpp/cpp` | UART мост для связи со STM32, обработка бинарных сообщений |
| `wifi.hpp/cpp` | WiFi клиент, подключение к сети, управление состоянием |

**Ключевые функции:**
- Подключение к WiFi сети (настройки в `main.cpp`: WIFI_SSID, WIFI_PASS)
- Подключение к win-x64 через TCP (настройки: SERVER_IP, SERVER_PORT)
- Маршрутизация сообщений TCP ↔ UART
- Обработка команд TEST и STATUS от win-x64
- Обработка тестовых изображений (пока возвращает оригинал без изменений)

**Для ИИ-агента:**
- 🔍 **Проблемы с WiFi**: проверьте `wifi.cpp` - функции `init()`, `wait_for_connection()`
- 🔍 **Проблемы с TCP**: проверьте `tcp.cpp` - функции `start()`, `tcp_task()`, состояние `TCP_STATE_CONNECTING`
- 🔍 **Проблемы с UART**: проверьте `uart_bridge.cpp` - функции `init()`, `uart_rx_task()`, настройки пинов (TX=17, RX=16)
- 🔍 **Ошибки маршрутизации**: проверьте `message_bridge.cpp` - функции `route_from_tcp()`, `route_from_uart()`, `process_buffer()`
- 🔍 **Проблемы с протоколом**: проверьте `message_bridge.cpp` - функция `process_buffer()` использует `msg_unpack()` из `message_protocol.h`

**Важные настройки в main.cpp:**
```cpp
#define WIFI_SSID "ModelSim"        // Имя WiFi сети
#define WIFI_PASS "adminadmin"      // Пароль WiFi
#define SERVER_IP   "192.168.43.236" // IP адрес win-x64
#define SERVER_PORT 1234             // TCP порт
```

---

### 3. stm32f4_cubemx_example (STM32 микроконтроллер)

**Назначение:** Периферийное устройство, обрабатывает команды и данные от ESP32 через UART.

**Основные компоненты:**
- **uart_handler** - обработка UART данных (C++ класс с C-обертками)
- **message_queue** - очередь сообщений для асинхронной обработки
- **message_protocol** - поддержка нового протокола сообщений

**Файлы:**

| Файл | Назначение |
|------|-----------|
| `Core/Src/main.c` | Точка входа, инициализация HAL, обработка UART1/UART2, старый CMD протокол |
| `Core/Inc/message_protocol.h` | Определения нового протокола сообщений (общий для всех модулей) |
| `Core/Src/message_protocol.c` | Реализация функций упаковки/распаковки сообщений |
| `Core/Inc/uart_handler.hpp` | C++ класс для обработки UART с поддержкой нового протокола |
| `Core/Src/uart_handler.cpp` | Реализация обработчика UART |
| `Core/Inc/uart_handler_c_wrapper.h` | C-обертки для использования C++ классов из C кода |
| `Core/Src/uart_handler_c_wrapper.cpp` | Реализация C-оберток |
| `Core/Inc/message_queue.hpp` | C++ класс очереди сообщений |
| `Core/Src/message_queue.cpp` | Реализация очереди сообщений |
| `Core/Inc/message_queue_c_wrapper.h` | C-обертки для message_queue |
| `Core/Src/message_queue_c_wrapper.cpp` | Реализация C-оберток |

**Текущее состояние:**
- ✅ Реализован старый CMD протокол (в `main.c`)
- ✅ Добавлены заготовки для нового протокола (C++ классы с C-обертками)
- ⚠️ Новый протокол пока не активирован (закомментирован в `main.c`)

**Для ИИ-агента:**
- 🔍 **Проблемы с UART**: проверьте `main.c` - функции `uart1rx_cb()`, `uart2rx_cb()`, настройки `huart1` и `huart2`
- 🔍 **Ошибки обработки команд**: проверьте `main.c` - функция `cmd_proc()`, обработка `CMD_STM`, `CMD_DPL`, `CMD_SPK`
- 🔍 **Активация нового протокола**: раскомментируйте код в `main.c` (строки 334-361) для использования `uart_handler` и `message_queue`
- 🔍 **Проблемы с C++ классами**: проверьте C-обертки в `uart_handler_c_wrapper.cpp` и `message_queue_c_wrapper.cpp`
- 🔍 **Ошибки компиляции**: убедитесь, что C++ файлы добавлены в проект Keil (`.uvprojx`)

**UART конфигурация:**
- **UART1**: Консольный интерфейс (для отладки)
- **UART2**: Связь с ESP32 (115200 baud, 8N1)

---

### 4. pyapp (Python тестовый клиент)

**Назначение:** Тестовое приложение для проверки работы всех компонентов системы.

**Файлы:**

| Файл | Назначение |
|------|-----------|
| `main.py` | Основной скрипт, последовательное тестирование всех компонентов |
| `vkorobka_client.py` | UDP клиент для связи с win-x64, отправка/прием JSON сообщений |
| `image_utils.py` | Утилиты для работы с изображениями (генерация, отображение, проверка) |

**Ключевые функции:**
- Генерация тестовых JPG изображений
- Отправка изображений на компоненты (win, esp32, stm32)
- Получение и проверка ответов
- Визуализация результатов

**Для ИИ-агента:**
- 🔍 **Проблемы с UDP**: проверьте `vkorobka_client.py` - функции `send_test_message()`, `_receive_loop()`
- 🔍 **Ошибки JSON**: проверьте `vkorobka_client.py` - функции `send_test_message()`, `_handle_response()`
- 🔍 **Проблемы с изображениями**: проверьте `image_utils.py` - функции `generate_test_image()`, `base64_to_image()`
- 🔍 **Таймауты**: увеличьте `timeout` в конструкторе `VkorobkaClient` (по умолчанию 10 секунд)

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

#### 3. UART не работает между ESP32 и STM32

**Проверьте:**
1. `esp32s3/main/uart_bridge.cpp` - инициализация UART, настройки пинов (TX=17, RX=16)
2. `esp32s3/main/message_bridge.cpp` - маршрутизация сообщений к UART
3. `stm32f4_cubemx_example/Core/Src/main.c` - обработка UART2, функция `uart2rx_cb()`
4. Физическое соединение: TX ESP32 → RX STM32, RX ESP32 → TX STM32
5. Скорость передачи: 115200 baud на обеих сторонах

#### 4. Ошибки обработки изображений

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
- UART STM32: `stm32f4_cubemx_example/Core/Src/main.c` (функции `uart1rx_cb()`, `uart2rx_cb()`)

**При ошибках памяти на STM32**
- Применить `Stack_Size		EQU     0x1000` в `startup_stm32f401xc.s` и `ProjectManager.StackSize=0x1000` в *.ioc файле

### Важные константы и настройки

**Порты:**
- UDP (Python ↔ win-x64): 1236
- TCP (win-x64 ↔ ESP32): 1234

**UART настройки:**
- Скорость: 115200 baud
- ESP32 пины: TX=17, RX=16 (UART_NUM_2)
- STM32: USART2

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

### stm32f4_cubemx_example

```bash
# Откройте stm32f4_cubemx_example.uvprojx в Keil MDK
# Соберите проект (F7)
# Загрузите в микроконтроллер (F8)
```

**Требования:**
- Keil MDK-ARM
- STM32F401CCUx микроконтроллер
- ST-Link или другой программатор

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

**Старый CMD протокол** (используется в STM32 для обратной совместимости):
- Заголовок: 6 байт (2 байта код команды + 4 байта длина)
- Коды команд: `CMD_WIN=0x0041`, `CMD_ESP=0x0048`, `CMD_STM=0x0049`

**Новый message_protocol** (используется везде):
- Заголовок: 12 байт (см. `message_protocol.h`)
- Типы сообщений: COMMAND, DATA, STREAM, RESPONSE, ERROR
- Поддержка приоритетов, потоков, sequence номеров

### Тестирование

Python клиент (`pyapp/main.py`) автоматически тестирует все компоненты:
1. Генерирует тестовое JPG изображение (4 КБ)
2. Отправляет на каждый компонент (win, esp32, stm32)
3. Проверяет, что получен отзеркаленный ответ
4. Выводит результаты тестирования

---

## 📄 Лицензия

Проект использует различные лицензии для разных компонентов:
- ESP-IDF компоненты: Apache 2.0
- STM32 HAL: BSD 3-Clause
- Пользовательский код: см. соответствующие файлы

---

## 👥 Контакты и поддержка

При возникновении проблем:
1. Проверьте логи всех компонентов
2. Убедитесь, что все настройки (IP, порты, WiFi) корректны
3. Проверьте физические соединения (UART между ESP32 и STM32)
4. Используйте раздел "Отладка и поиск ошибок" выше

---

*Последнее обновление: 2024*

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

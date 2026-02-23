# Рекомендации по отладке и breakpoints для STM32

## Отладочные символы на UART1

При работе программы на UART1 отправляются следующие символы:
- **I** - Инициализация успешна
- **R** - Получен байт на UART1 (отладка)
- **M** - Сообщение обработано в callback
- **E** - Ошибка (инициализация или другая)
- **V** - Невалидный заголовок сообщения
- **P** - Сообщение успешно распарсено
- **Q** - Сообщение добавлено в очередь
- **C** - Начата обработка очереди сообщений
- **S** - Ответ отправлен успешно
- **3** - destination_id = MSG_DST_STM32 (0x03)
- **B** - destination_id = MSG_DST_BROADCAST (0xFF)
- **X** - destination_id не для STM32 (сообщение игнорируется)
- **1-9** - Размер очереди перед обработкой
- **+** - Размер очереди > 9
- **D** - Попытка извлечь сообщение из очереди
- **E** - Сообщение успешно извлечено из очереди
- **0-9, A-F** - Тип сообщения (msg_type)

## Критические breakpoints для отладки

### 1. Прием данных из прерывания
**Файл:** `Core/Src/main.c`  
**Функция:** `uart2rx_cb()`  
**Строка:** ~182 (после `uart_handler_push_rx_data`)  
**Проверяемые значения:**
- `uart2_rx_byte` - полученный байт (должен быть валидным)
- `g_uart_handler` - должен быть != NULL

### 2. Добавление данных в буфер
**Файл:** `Core/Src/uart_handler.cpp`  
**Функция:** `uart_handler_t::push_rx_data()`  
**Строка:** ~174 (после `rx_buffer_.push(data, len)`)  
**Проверяемые значения:**
- `data[0]` - первый байт данных
- `len` - длина данных (должна быть 1 для байта из прерывания)
- `rx_buffer_.available()` - размер данных в буфере

### 3. Парсинг заголовка
**Файл:** `Core/Src/uart_handler.cpp`  
**Функция:** `uart_handler_t::process_rx_buffer()`  
**Строка:** ~201 (после `msg_unpack`)  
**Проверяемые значения:**
- `header_buffer[0..11]` - сырые байты заголовка
- `header.msg_type` - должен быть 0x01-0x05
- `header.source_id` - должен быть 0x01-0x04
- `header.destination_id` - должен быть 0x01-0x04 или 0xFF
- `header.payload_len` - длина payload (не должна быть огромной)

### 4. Валидация заголовка
**Файл:** `Core/Src/uart_handler.cpp`  
**Функция:** `uart_handler_t::process_rx_buffer()`  
**Строка:** ~219 (после `msg_validate_header`)  
**Проверяемые значения:**
- `valid` - результат валидации (должен быть 1 для валидного)
- `header.msg_type` - проверка диапазона
- `header.source_id` - проверка диапазона
- `header.destination_id` - проверка диапазона
- `header.payload_len` - проверка размера

### 5. Вызов обработчика
**Файл:** `Core/Src/main.c`  
**Функция:** `handle_message_callback()`  
**Строка:** ~75 (начало функции)  
**Проверяемые значения:**
- `header->destination_id` - должен быть MSG_DST_STM32 или MSG_DST_BROADCAST
- `header->msg_type` - тип сообщения
- `payload_len` - длина payload

### 6. Обработка очереди
**Файл:** `Core/Src/main.c`  
**Функция:** `process_message_queue()`  
**Строка:** ~114 (после `message_queue_dequeue`)  
**Проверяемые значения:**
- `header.destination_id` - должен быть MSG_DST_STM32
- `header.msg_type` - тип сообщения (MSG_TYPE_DATA для теста)
- `payload_len` - длина payload
- `payload_buffer[0..N]` - содержимое payload

### 7. Отправка ответа
**Файл:** `Core/Src/main.c`  
**Функция:** `process_message_queue()`  
**Строка:** ~139 (после `uart_handler_send_message`)  
**Проверяемые значения:**
- Результат `uart_handler_send_message()` - должен быть 1
- `response.msg_type` - должен быть MSG_TYPE_RESPONSE
- `response.destination_id` - должен быть MSG_DST_EXTERNAL

## Типичные проблемы и где их искать

### Проблема: Невалидный заголовок (destination_id = 0xaf)
**Где искать:**
- Breakpoint в `process_rx_buffer()` строка ~201 (после `msg_unpack`)
- Проверить `header_buffer[]` - возможно данные не синхронизированы
- Проверить `rx_buffer_.available()` - возможно буфер содержит мусор

### Проблема: Огромный payload_len (0x1483c1e0)
**Где искать:**
- Breakpoint в `process_rx_buffer()` строка ~201
- Проверить `header_buffer[7..10]` - это байты payload_len в little-endian
- Возможно данные приходят в неправильном формате или не синхронизированы

### Проблема: Только символ "M" в логе
**Где искать:**
- Breakpoint в `handle_message_callback()` - проверять, вызывается ли функция
- Breakpoint в `process_message_queue()` - проверять, обрабатывается ли очередь
- Проверить `g_message_queue` - не NULL ли

### Проблема: Нет ответа на тест
**Где искать:**
- Breakpoint в `process_message_queue()` строка ~119 - проверять условие `destination_id == MSG_DST_STM32`
- Breakpoint в `process_message_queue()` строка ~139 - проверять отправку ответа
- Проверить `uart_handler_send_message()` - возвращает ли 1

## Порядок отладки

1. **Проверить инициализацию:**
   - Breakpoint в `main()` строка ~266 (после `get_uart_handler_instance`)
   - Убедиться, что `g_uart_handler != NULL`
   - Проверить отправку символа "I" на UART1

2. **Проверить прием данных:**
   - Breakpoint в `uart2rx_cb()` строка ~182
   - Убедиться, что данные приходят
   - Проверить, что `uart_handler_push_rx_data()` вызывается

3. **Проверить парсинг:**
   - Breakpoint в `process_rx_buffer()` строка ~201
   - Проверить сырые байты `header_buffer[]`
   - Проверить результат `msg_unpack()`

4. **Проверить валидацию:**
   - Breakpoint в `process_rx_buffer()` строка ~219
   - Проверить результат `msg_validate_header()`
   - Проверить значения полей заголовка

5. **Проверить обработку:**
   - Breakpoint в `handle_message_callback()`
   - Проверить `destination_id` - должен быть MSG_DST_STM32
   - Проверить добавление в очередь

6. **Проверить отправку ответа:**
   - Breakpoint в `process_message_queue()` строка ~139
   - Проверить результат отправки
   - Проверить отправку символа "S"

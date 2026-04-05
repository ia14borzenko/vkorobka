#pragma once

#include "ili9486_display.hpp"
#include "my_types.h"

constexpr u16 LCD_STREAM_ID_FRAME = 1;

// Структура для хранения одного чанка в буфере
struct LcdChunk
{
    u8* data;          // Выделенная память для данных чанка (освобождается после вывода)
    u32 data_len;      // Размер данных в байтах
    u16 y_start;       // Начальная Y-координата этого чанка
    u16 height_chunk;  // Количество строк в этом чанке
    bool valid;        // Флаг валидности (true = чанк готов к выводу)
};

// Состояние потокового вывода кадра на дисплей (с буферизацией для переупорядочивания)
struct LcdStreamState
{
    u16 width;         // Ширина кадра (480 для MAR3501)
    u16 height;        // Высота кадра (320 для MAR3501)
    u16 total_chunks;  // Общее количество чанков в кадре
    u16 received_chunks; // Количество полученных чанков
    u16 format_code;   // Код формата (1 = RGB565_BE)
    u16 next_y;        // Следующая ожидаемая Y-координата для вывода (для проверки порядка)
    bool active;       // Флаг активного потока (true = идёт передача кадра)
    
    // Буфер для переупорядочивания чанков (максимум 32 чанка)
    static constexpr u16 MAX_BUFFERED_CHUNKS = 32;
    LcdChunk buffered_chunks[MAX_BUFFERED_CHUNKS];
    u16 buffered_count;  // Текущее количество буферизованных чанков
};

// Инициализация модуля потоковой обработки
void stream_handler_init(Ili9486Display* lcd);

// Обработка потокового чанка
void stream_handler_process_chunk(const u8* payload, u32 payload_len, u16 stream_id, u32 sequence);

// Очистка состояния потока
void stream_handler_cleanup();

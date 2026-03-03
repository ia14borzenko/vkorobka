#pragma once

#include "ili9486_display.hpp"
#include "my_types.h"

// Структура для хранения символа в буфере
struct CharData
{
    u32 unicode_code; // Unicode код символа (например, 0x0041 для 'A')
    u8* rgb565_data;  // RGB565 данные символа (big-endian)
    u16 width;        // Ширина символа в пикселях
    u16 height;       // Высота символа в пикселях
    u32 data_size;    // Размер данных в байтах (width * height * 2)
    bool valid;       // Флаг валидности
};

// Состояние текстинга
struct TextingState
{
    // Параметры поля
    u16 field_x;
    u16 field_y;
    u16 field_width;
    u16 field_height;
    u16 char_height;
    u16 line_spacing;
    u16 typing_speed_ms;
    
    // Позиция курсора
    u16 cursor_x;
    u16 cursor_y;
    u16 current_line_index;
    u16 max_lines;
    
    // Буфер символов (максимум 256 символов)
    static constexpr u16 MAX_CHARS = 256;
    CharData chars[MAX_CHARS];
    u16 chars_count;
    
    // Текст для вывода
    char* text;
    u32 text_len;
    
    bool active;  // Флаг активного текстинга
};

// Инициализация модуля текстинга
void texting_init(Ili9486Display* lcd);

// Обработка команды TEXT_CLEAR
void texting_handle_clear(const char* json_buf, u32 json_len);

// Обработка команды TEXT_ADD
void texting_handle_add(const char* json_buf, u32 json_len);

// Очистка состояния текстинга
void texting_cleanup();

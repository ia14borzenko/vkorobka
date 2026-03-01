#include "texting.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "message_bridge.hpp"
#include "message_protocol.h"
#include "my_types.h"

static const char* TAG = "texting";

extern Ili9486Display* s_lcd;  // Глобальный указатель на дисплей из main.cpp
static TextingState s_texting_state;
extern message_bridge_t* g_message_bridge;

// Простая реализация декодирования base64
static int base64_decode(const char* input, size_t input_len, u8* output, size_t* output_len) {
    static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, valb = -8;
    size_t out_idx = 0;
    
    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '=') break;
        
        const char* p = strchr(base64_chars, c);
        if (p == NULL) continue;
        
        val = (val << 6) + (p - base64_chars);
        valb += 6;
        
        if (valb >= 0) {
            if (out_idx >= *output_len) {
                return -1; // Переполнение буфера
            }
            output[out_idx++] = (val >> valb) & 0xFF;
            valb -= 8;
        }
    }
    
    *output_len = out_idx;
    return 0;
}

// Вспомогательная функция для парсинга числа из JSON (учитывает пробелы)
static int parse_json_int(const char* field_name, const char* json_buf, int default_val) {
    char search_str[64];
    snprintf(search_str, sizeof(search_str), "\"%s\"", field_name);
    const char* field = strstr(json_buf, search_str);
    if (!field) return default_val;
    const char* colon = strchr(field, ':');
    if (!colon) return default_val;
    const char* val = colon + 1;
    while (*val == ' ' || *val == '\t') val++;
    int result = default_val;
    sscanf(val, "%d", &result);
    return result;
}

// Функция для поиска символа в буфере по Unicode коду
static CharData* find_char_data(u32 unicode_code) {
    for (u16 i = 0; i < s_texting_state.chars_count; ++i) {
        if (s_texting_state.chars[i].valid && s_texting_state.chars[i].unicode_code == unicode_code) {
            return &s_texting_state.chars[i];
        }
    }
    return nullptr;
}

// Задача для вывода текста с имитацией текстинга
static void texting_render_task(void* pvParameters) {
    ESP_LOGI(TAG, "Render task started");
    
    if (!s_texting_state.active || !s_texting_state.text || !s_lcd) {
        ESP_LOGE(TAG, "Invalid state for rendering");
        vTaskDelete(nullptr);
        return;
    }
    
    u16 cursor_x = s_texting_state.cursor_x;
    u16 cursor_y = s_texting_state.cursor_y;
    u16 line_index = s_texting_state.current_line_index;
    
    // Простой вывод текста посимвольно
    for (u32 i = 0; i < s_texting_state.text_len; ++i) {
        // Обработка UTF-8 символов
        u32 unicode = 0;
        unsigned char c = (unsigned char)s_texting_state.text[i];
        
        if ((c & 0x80) == 0) {
            // ASCII символ (1 байт)
            unicode = c;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-байтовый UTF-8 символ
            if (i + 1 < s_texting_state.text_len) {
                unicode = ((c & 0x1F) << 6) | (s_texting_state.text[i + 1] & 0x3F);
                i++; // Пропускаем следующий байт
            } else {
                continue; // Неполный символ
            }
        } else if ((c & 0xF0) == 0xE0) {
            // 3-байтовый UTF-8 символ (кириллица)
            if (i + 2 < s_texting_state.text_len) {
                unicode = ((c & 0x0F) << 12) | 
                         ((s_texting_state.text[i + 1] & 0x3F) << 6) |
                         (s_texting_state.text[i + 2] & 0x3F);
                i += 2; // Пропускаем следующие байты
            } else {
                continue; // Неполный символ
            }
        } else {
            // Неподдерживаемый формат или продолжение байта
            continue;
        }
        
        // Ищем символ в буфере
        CharData* char_data = find_char_data(unicode);
        
        if (!char_data || !char_data->valid) {
            ESP_LOGW(TAG, "Char (U+%04X) not found, skipping", unicode);
            continue;
        }
        
        // Проверяем переполнение поля
        if (line_index >= s_texting_state.max_lines) {
            // Очищаем первую строку и начинаем заново
            s_lcd->fillRect(s_texting_state.field_x, s_texting_state.field_y,
                           s_texting_state.field_width, s_texting_state.char_height + s_texting_state.line_spacing,
                           0xFFFF);
            cursor_x = 0;
            cursor_y = 0;
            line_index = 0;
        }
        
        // Проверяем, влезает ли символ в текущую строку
        if (cursor_x + char_data->width > s_texting_state.field_width) {
            // Переходим на следующую строку
            cursor_x = 0;
            cursor_y += s_texting_state.char_height + s_texting_state.line_spacing;
            line_index++;
            
            if (line_index >= s_texting_state.max_lines) {
                // Очищаем первую строку
                s_lcd->fillRect(s_texting_state.field_x, s_texting_state.field_y,
                               s_texting_state.field_width, s_texting_state.char_height + s_texting_state.line_spacing,
                               0xFFFF);
                cursor_x = 0;
                cursor_y = 0;
                line_index = 0;
            }
        }
        
        // Вычисляем абсолютные координаты
        u16 screen_x = s_texting_state.field_x + cursor_x;
        u16 screen_y = s_texting_state.field_y + cursor_y;
        
        // Выводим символ
        ESP_LOGI(TAG, "Drawing char U+%04X at (%u, %u), size=%ux%u", 
                 unicode, screen_x, screen_y, char_data->width, char_data->height);
        s_lcd->drawChar(char_data->rgb565_data, char_data->width, char_data->height, screen_x, screen_y);
        
        // Обновляем позицию курсора
        cursor_x += char_data->width;
        
        // Задержка между символами
        if (s_texting_state.typing_speed_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_texting_state.typing_speed_ms));
        }
    }
    
    ESP_LOGI(TAG, "Text rendering completed");
    s_texting_state.active = false;
    vTaskDelete(nullptr);
}

void texting_init(Ili9486Display* lcd) {
    s_lcd = lcd;
    memset(&s_texting_state, 0, sizeof(s_texting_state));
}

void texting_handle_clear(const char* json_buf, u32 json_len) {
    ESP_LOGI(TAG, "Processing TEXT_CLEAR command");
    
    // Парсим параметры поля
    int field_x = 52, field_y = 160, field_width = 380, field_height = 120;
    
    // Простой парсинг JSON значений
    const char* x_str = strstr(json_buf, "\"field_x\":");
    const char* y_str = strstr(json_buf, "\"field_y\":");
    const char* w_str = strstr(json_buf, "\"field_width\":");
    const char* h_str = strstr(json_buf, "\"field_height\":");
    
    if (x_str) sscanf(x_str, "\"field_x\":%d", &field_x);
    if (y_str) sscanf(y_str, "\"field_y\":%d", &field_y);
    if (w_str) sscanf(w_str, "\"field_width\":%d", &field_width);
    if (h_str) sscanf(h_str, "\"field_height\":%d", &field_height);
    
    ESP_LOGI(TAG, "Clearing field: x=%d, y=%d, w=%d, h=%d", field_x, field_y, field_width, field_height);
    
    // Очищаем поле белым цветом (RGB565: 0xFFFF)
    if (s_lcd) {
        s_lcd->fillRect(field_x, field_y, field_width, field_height, 0xFFFF);
        ESP_LOGI(TAG, "Field cleared successfully");
    }
    
    // Отправляем подтверждение
    const char* response = "TEXT_CLEAR_OK";
    msg_header_t response_header = msg_create_header(
        MSG_TYPE_RESPONSE,
        MSG_SRC_ESP32,
        MSG_DST_EXTERNAL,
        128,
        0,
        strlen(response),
        0,
        MSG_ROUTE_NONE
    );
    
    if (g_message_bridge && g_message_bridge->send_message(response_header, 
                                                            reinterpret_cast<const u8*>(response), 
                                                            strlen(response)))
    {
        ESP_LOGI(TAG, "TEXT_CLEAR response sent");
    }
}

void texting_handle_add(const char* json_buf, u32 json_len) {
    ESP_LOGI(TAG, "Processing TEXT_ADD command");
    
    // Парсим параметры
    int field_x = 52, field_y = 160, field_width = 380, field_height = 120;
    int char_height = 14, line_spacing = 2, typing_speed_ms = 50;
    int cursor_x = 0, cursor_y = 0, current_line_index = 0;
    
    // Парсим параметры поля (учитываем пробелы в JSON)
    field_x = parse_json_int("field_x", json_buf, field_x);
    field_y = parse_json_int("field_y", json_buf, field_y);
    field_width = parse_json_int("field_width", json_buf, field_width);
    field_height = parse_json_int("field_height", json_buf, field_height);
    char_height = parse_json_int("char_height", json_buf, char_height);
    line_spacing = parse_json_int("line_spacing", json_buf, line_spacing);
    typing_speed_ms = parse_json_int("typing_speed_ms", json_buf, typing_speed_ms);
    cursor_x = parse_json_int("cursor_x", json_buf, cursor_x);
    cursor_y = parse_json_int("cursor_y", json_buf, cursor_y);
    current_line_index = parse_json_int("current_line_index", json_buf, current_line_index);
    
    // Парсим текст (учитываем пробелы в JSON: "text": "Hello" или "text":"Hello")
    const char* text_field = strstr(json_buf, "\"text\"");
    if (!text_field) {
        ESP_LOGE(TAG, "TEXT_ADD: text field not found in JSON");
        return;
    }
    
    // Ищем двоеточие после "text"
    const char* colon = strchr(text_field, ':');
    if (!colon) {
        ESP_LOGE(TAG, "TEXT_ADD: malformed text field (no colon)");
        return;
    }
    
    // Пропускаем пробелы после двоеточия
    const char* text_start = colon + 1;
    while (*text_start == ' ' || *text_start == '\t') {
        text_start++;
    }
    
    // Проверяем, что следующая кавычка
    if (*text_start != '"') {
        ESP_LOGE(TAG, "TEXT_ADD: malformed text field (no opening quote)");
        return;
    }
    
    text_start++; // Пропускаем открывающую кавычку
    const char* text_end = strchr(text_start, '"');
    if (!text_end) {
        ESP_LOGE(TAG, "TEXT_ADD: malformed text field (no closing quote)");
        return;
    }
    
    size_t text_len = text_end - text_start;
    if (text_len > 1000) {
        ESP_LOGE(TAG, "TEXT_ADD: text too long (%zu chars)", text_len);
        return;
    }
    
    char* text = (char*)malloc(text_len + 1);
    if (!text) {
        ESP_LOGE(TAG, "TEXT_ADD: failed to allocate memory for text");
        return;
    }
    
    memcpy(text, text_start, text_len);
    text[text_len] = '\0';
    
    ESP_LOGI(TAG, "TEXT_ADD: text='%s' (len=%zu)", text, text_len);
    ESP_LOGI(TAG, "TEXT_ADD: field=(%d,%d) %dx%d, char_h=%d, spacing=%d, speed=%dms",
             field_x, field_y, field_width, field_height, char_height, line_spacing, typing_speed_ms);
    
    // Очищаем предыдущее состояние
    if (s_texting_state.active && s_texting_state.text) {
        free(s_texting_state.text);
    }
            for (u16 i = 0; i < s_texting_state.chars_count; ++i) {
                if (s_texting_state.chars[i].valid && s_texting_state.chars[i].rgb565_data) {
                    heap_caps_free(s_texting_state.chars[i].rgb565_data);
                }
            }
    
    // Инициализируем состояние
    s_texting_state.field_x = field_x;
    s_texting_state.field_y = field_y;
    s_texting_state.field_width = field_width;
    s_texting_state.field_height = field_height;
    s_texting_state.char_height = char_height;
    s_texting_state.line_spacing = line_spacing;
    s_texting_state.typing_speed_ms = typing_speed_ms;
    s_texting_state.cursor_x = cursor_x;
    s_texting_state.cursor_y = cursor_y;
    s_texting_state.current_line_index = current_line_index;
    s_texting_state.max_lines = (field_height + line_spacing) / (char_height + line_spacing);
    s_texting_state.chars_count = 0;
    s_texting_state.text = text;
    s_texting_state.text_len = text_len;
    s_texting_state.active = true;
    
    // Парсим символы из JSON
    const char* chars_field = strstr(json_buf, "\"chars\"");
    if (!chars_field) {
        ESP_LOGE(TAG, "'chars' field not found in JSON!");
    } else {
        ESP_LOGI(TAG, "Found 'chars' field at offset %zu", chars_field - json_buf);
        
        // Ищем двоеточие после "chars"
        const char* chars_colon = strchr(chars_field, ':');
        if (!chars_colon) {
            ESP_LOGE(TAG, "No colon after 'chars' field");
        } else {
            // Пропускаем пробелы после двоеточия
            const char* chars_start = chars_colon + 1;
            while (*chars_start == ' ' || *chars_start == '\t') chars_start++;
            
            if (*chars_start != '{') {
                ESP_LOGE(TAG, "'chars' value doesn't start with '{', got '%c'", *chars_start);
            } else {
                chars_start++; // Пропускаем '{'
                ESP_LOGI(TAG, "Parsing characters from JSON...");
                ESP_LOGI(TAG, "chars section starts at offset %zu", chars_start - json_buf);
                
                // Ищем закрывающую скобку (нужно найти правильную '}', учитывая вложенность)
                const char* chars_end = chars_start;
                int brace_count = 1;
                while (*chars_end != '\0' && brace_count > 0) {
                    if (*chars_end == '{') brace_count++;
                    else if (*chars_end == '}') brace_count--;
                    chars_end++;
                }
                if (brace_count > 0) {
                    ESP_LOGW(TAG, "chars section not properly closed (brace_count=%d)", brace_count);
                    chars_end = nullptr;
                } else {
                    chars_end--; // Откатываемся на закрывающую '}'
                    ESP_LOGI(TAG, "chars section ends at offset %zu, length=%zu", 
                             chars_end - json_buf, chars_end - chars_start);
                }
        
                const char* pos = chars_start;
                int char_parse_count = 0;
                while (pos && *pos != '\0' && pos < chars_end && s_texting_state.chars_count < TextingState::MAX_CHARS) {
                    char_parse_count++;
                    if (char_parse_count > 100) {
                        ESP_LOGW(TAG, "Too many parse iterations, breaking");
                        break;
                    }
                    
                    // Ищем начало следующего символа: "U+XXXX":
                    const char* unicode_start = strstr(pos, "\"U+");
                    if (!unicode_start) {
                        ESP_LOGI(TAG, "No more 'U+' found, parsed %d chars so far", s_texting_state.chars_count);
                        break; // Больше нет символов
                    }
                    if (chars_end && unicode_start > chars_end) {
                        ESP_LOGI(TAG, "Unicode start beyond chars_end, breaking");
                        break; // Вышли за границы секции chars
                    }
                    
                    ESP_LOGI(TAG, "Found Unicode key at offset %zu", unicode_start - json_buf);
                    
                    unicode_start += 1; // Пропускаем первую кавычку
                    const char* unicode_end = strchr(unicode_start, '"');
                    if (!unicode_end) {
                        ESP_LOGW(TAG, "No closing quote for Unicode key");
                        break;
                    }
                    
                    // Извлекаем Unicode код
                    char unicode_code[16];
                    size_t unicode_len = unicode_end - unicode_start;
                    if (unicode_len >= sizeof(unicode_code)) {
                        ESP_LOGW(TAG, "Unicode code too long");
                        pos = unicode_end + 1;
                        continue;
                    }
                    memcpy(unicode_code, unicode_start, unicode_len);
                    unicode_code[unicode_len] = '\0';
                    
                    ESP_LOGI(TAG, "Parsing char %s", unicode_code);
                    
                    // Ищем width, height, data (учитываем пробелы)
                    const char* width_field = strstr(unicode_end, "\"width\"");
                    const char* height_field = strstr(unicode_end, "\"height\"");
                    const char* data_field = strstr(unicode_end, "\"data\"");
                    
                    if (!width_field || !height_field || !data_field) {
                        ESP_LOGW(TAG, "Missing width/height/data fields for %s", unicode_code);
                        pos = unicode_end + 1;
                        continue;
                    }
                    
                    // Парсим width
                    const char* width_colon = strchr(width_field, ':');
                    if (!width_colon) {
                        ESP_LOGW(TAG, "No colon in width field for %s", unicode_code);
                        pos = unicode_end + 1;
                        continue;
                    }
                    const char* width_val = width_colon + 1;
                    while (*width_val == ' ' || *width_val == '\t') width_val++;
                    int char_width = 0;
                    if (sscanf(width_val, "%d", &char_width) != 1 || char_width <= 0) {
                        ESP_LOGW(TAG, "Invalid width for %s", unicode_code);
                        pos = unicode_end + 1;
                        continue;
                    }
                    
                    // Парсим height
                    const char* height_colon = strchr(height_field, ':');
                    if (!height_colon) {
                        ESP_LOGW(TAG, "No colon in height field for %s", unicode_code);
                        pos = unicode_end + 1;
                        continue;
                    }
                    const char* height_val = height_colon + 1;
                    while (*height_val == ' ' || *height_val == '\t') height_val++;
                    int char_height_val = 0;
                    if (sscanf(height_val, "%d", &char_height_val) != 1 || char_height_val <= 0) {
                        ESP_LOGW(TAG, "Invalid height for %s", unicode_code);
                        pos = unicode_end + 1;
                        continue;
                    }
                    
                    // Извлекаем base64 данные
                    const char* data_colon = strchr(data_field, ':');
                    if (!data_colon) {
                        ESP_LOGW(TAG, "No colon in data field for %s", unicode_code);
                        pos = unicode_end + 1;
                        continue;
                    }
                    const char* data_str = data_colon + 1;
                    while (*data_str == ' ' || *data_str == '\t') data_str++;
                    if (*data_str != '"') {
                        ESP_LOGW(TAG, "Invalid data format for %s (expected quote, got '%c')", unicode_code, *data_str);
                        pos = unicode_end + 1;
                        continue;
                    }
                    data_str++; // Пропускаем открывающую кавычку
                    const char* data_end = strchr(data_str, '"');
                    if (!data_end) {
                        ESP_LOGW(TAG, "No closing quote for data in %s", unicode_code);
                        pos = data_str + 1;
                        continue;
                    }
                    
                    size_t base64_len = data_end - data_str;
                    if (base64_len == 0 || base64_len > 10000) {
                        ESP_LOGW(TAG, "Invalid base64 length for %s: %zu", unicode_code, base64_len);
                        pos = data_end + 1;
                        continue;
                    }
                    
                    // Декодируем base64
                    size_t rgb565_size = char_width * char_height_val * 2;
                    u8* rgb565_data = (u8*)heap_caps_malloc(rgb565_size, MALLOC_CAP_8BIT);
                    
                    if (rgb565_data) {
                        size_t decoded_len = rgb565_size;
                        int ret = base64_decode(data_str, base64_len, rgb565_data, &decoded_len);
                        if (ret == 0 && decoded_len == rgb565_size) {
                            // Парсим Unicode код из строки "U+XXXX"
                            u32 unicode_val = 0;
                            if (sscanf(unicode_code, "U+%X", &unicode_val) == 1) {
                                // Сохраняем символ
                                CharData& char_data = s_texting_state.chars[s_texting_state.chars_count];
                                char_data.unicode_code = unicode_val;
                                char_data.rgb565_data = rgb565_data;
                                char_data.width = char_width;
                                char_data.height = char_height_val;
                                char_data.data_size = rgb565_size;
                                char_data.valid = true;
                                
                                s_texting_state.chars_count++;
                                ESP_LOGI(TAG, "Loaded char %s (U+%04X): %dx%d, data_size=%u", 
                                         unicode_code, unicode_val, char_width, char_height_val, rgb565_size);
                            } else {
                                ESP_LOGW(TAG, "Failed to parse Unicode code: %s", unicode_code);
                                heap_caps_free(rgb565_data);
                            }
                        } else {
                            ESP_LOGW(TAG, "Failed to decode base64 for %s (ret=%d, decoded=%zu, expected=%zu)",
                                     unicode_code, ret, decoded_len, rgb565_size);
                            heap_caps_free(rgb565_data);
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to allocate memory for char %s", unicode_code);
                    }
                    
                    pos = data_end + 1;
                }
            } // закрывает else от строки 628
        } // закрывает else от строки 621
    } // закрывает else от строки 614
    
    ESP_LOGI(TAG, "Loaded %u characters, starting text rendering...", s_texting_state.chars_count);
    
    if (s_texting_state.chars_count == 0) {
        ESP_LOGW(TAG, "No characters loaded, cannot render text");
    } else {
        // Запускаем задачу для вывода текста
        BaseType_t task_result = xTaskCreate(texting_render_task, "texting_render", 8192, nullptr, 5, nullptr);
        if (task_result == pdPASS) {
            ESP_LOGI(TAG, "Render task created successfully");
        } else {
            ESP_LOGE(TAG, "Failed to create render task (result=%d)", task_result);
        }
    }
    
    // Отправляем подтверждение
    const char* response = "TEXT_ADD_OK";
    msg_header_t response_header = msg_create_header(
        MSG_TYPE_RESPONSE,
        MSG_SRC_ESP32,
        MSG_DST_EXTERNAL,
        128,
        0,
        strlen(response),
        0,
        MSG_ROUTE_NONE
    );
    
    if (g_message_bridge && g_message_bridge->send_message(response_header, 
                                                            reinterpret_cast<const u8*>(response), 
                                                            strlen(response)))
    {
        ESP_LOGI(TAG, "TEXT_ADD response sent");
    }
}

void texting_cleanup() {
    if (s_texting_state.active && s_texting_state.text) {
        free(s_texting_state.text);
        s_texting_state.text = nullptr;
    }
    for (u16 i = 0; i < s_texting_state.chars_count; ++i) {
        if (s_texting_state.chars[i].valid && s_texting_state.chars[i].rgb565_data) {
            heap_caps_free(s_texting_state.chars[i].rgb565_data);
            s_texting_state.chars[i].valid = false;
        }
    }
    s_texting_state.chars_count = 0;
    s_texting_state.active = false;
}

#include "stream_handler.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "message_bridge.hpp"
#include "message_protocol.h"
#include "my_types.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "stream_handler";

extern Ili9486Display* s_lcd;  // Глобальный указатель на дисплей из main.cpp
static LcdStreamState s_lcd_stream;
extern message_bridge_t* g_message_bridge;

void stream_handler_init(Ili9486Display* lcd) {
    s_lcd = lcd;
    memset(&s_lcd_stream, 0, sizeof(s_lcd_stream));
}

void stream_handler_process_chunk(const u8* payload, u32 payload_len, u16 stream_id, u32 sequence) {
    if (stream_id != LCD_STREAM_ID_FRAME) {
        ESP_LOGW(TAG, "Unknown stream_id=%u, ignoring", stream_id);
        return;
    }
    
    ESP_LOGI(TAG, "STREAM frame chunk: len=%u", payload_len);

    // Внутренний заголовок в начале payload (новый формат для потокового вывода):
    // u16 width
    // u16 height
    // u16 chunk_index
    // u16 total_chunks
    // u16 format_code
    // u16 y_start          (начальная Y-координата для этого чанка)
    // u16 height_chunk     (количество строк в этом чанке)
    const u32 header_size = 2 + 2 + 2 + 2 + 2 + 2 + 2;  // 7 * u16 = 14 байт
    if (payload_len <= header_size)
    {
        ESP_LOGE(TAG, "STREAM chunk too small: %u (header_size=%u)", payload_len, header_size);
        return;
    }

    const u8* p = payload;
    u16 width  = (u16)p[0] | ((u16)p[1] << 8);
    u16 height = (u16)p[2] | ((u16)p[3] << 8);
    u16 chunk_index  = (u16)p[4]  | ((u16)p[5]  << 8);
    u16 total_chunks = (u16)p[6] | ((u16)p[7] << 8);
    u16 format_code  = (u16)p[8] | ((u16)p[9] << 8);
    u16 y_start      = (u16)p[10] | ((u16)p[11] << 8);
    u16 height_chunk = (u16)p[12] | ((u16)p[13] << 8);

    const u32 chunk_data_len = payload_len - header_size;

    ESP_LOGI(TAG,
             "STREAM hdr: %ux%u, chunk=%u/%u, fmt=%u, y_start=%u, h_chunk=%u, data_len=%u",
             width, height, chunk_index, total_chunks, format_code, y_start, height_chunk, chunk_data_len);

    if (width == 0 || height == 0 || total_chunks == 0 || chunk_index >= total_chunks)
    {
        ESP_LOGE(TAG, "STREAM header invalid, drop chunk");
        return;
    }

    // Поддерживаем только формат 1 = RGB565_BE
    if (format_code != 1)
    {
        ESP_LOGE(TAG, "Unsupported frame format_code=%u", format_code);
        return;
    }

    // Инициализация состояния при первом чанке
    if (!s_lcd_stream.active)
    {
        ESP_LOGI(TAG, "STREAM start new frame (streaming mode with buffering)");
        s_lcd_stream.width = width;
        s_lcd_stream.height = height;
        s_lcd_stream.total_chunks = total_chunks;
        s_lcd_stream.received_chunks = 0;
        s_lcd_stream.format_code = format_code;
        s_lcd_stream.next_y = 0;
        s_lcd_stream.buffered_count = 0;
        s_lcd_stream.active = true;
        
            // Инициализация буфера чанков
            for (u16 i = 0; i < LcdStreamState::MAX_BUFFERED_CHUNKS; ++i)
        {
            s_lcd_stream.buffered_chunks[i].valid = false;
            s_lcd_stream.buffered_chunks[i].data = nullptr;
        }
    }

    // Проверка консистентности
    if (width      != s_lcd_stream.width      ||
        height     != s_lcd_stream.height     ||
        total_chunks != s_lcd_stream.total_chunks ||
        format_code  != s_lcd_stream.format_code)
    {
        ESP_LOGE(TAG, "STREAM header mismatch with active frame, resetting state");
        s_lcd_stream = LcdStreamState();
        return;
    }

    // Буферизация и вывод чанков с переупорядочиванием
    if (s_lcd && chunk_data_len > 0)
    {
        const u8* chunk_data = payload + header_size;
        
        // Логика обработки чанков
        // Если чанк идёт по порядку (y_start == next_y) - выводим сразу на дисплей
        // Если чанк не по порядку - буферизуем его для последующего вывода
        if (y_start == s_lcd_stream.next_y)
        {
            // Чанк идёт по порядку - выводим сразу (потоковый режим, без накопления в памяти)
            s_lcd->drawRgb565Chunk(chunk_data, width, y_start, height_chunk);
            s_lcd_stream.next_y = y_start + height_chunk;
            ESP_LOGI(TAG, "STREAM chunk drawn immediately: y=%u-%u, next_y=%u", 
                     y_start, y_start + height_chunk - 1, s_lcd_stream.next_y);
            
            // Отправляем подтверждение приёма этого чанка (для flow control)
            char ack_buf[32];
            int ack_len = snprintf(ack_buf, sizeof(ack_buf), "CHUNK_ACK:%u", chunk_index);
            if (ack_len > 0 && ack_len < (int)sizeof(ack_buf))
            {
                msg_header_t ack_header = msg_create_header(
                    MSG_TYPE_RESPONSE,
                    MSG_SRC_ESP32,
                    MSG_DST_EXTERNAL,
                    128,
                    stream_id,
                    (u32)ack_len,
                    sequence,
                    MSG_ROUTE_NONE
                );
                
                if (g_message_bridge &&
                    g_message_bridge->send_message(ack_header, 
                                                   reinterpret_cast<const u8*>(ack_buf), 
                                                   (u32)ack_len))
                {
                    ESP_LOGI(TAG, "STREAM chunk ACK sent: chunk=%u, seq=%u", chunk_index, sequence);
                }
            }
            
            // Проверяем, можно ли вывести буферизованные чанки
            while (s_lcd_stream.buffered_count > 0)
            {
                    bool found_next = false;
                    for (u16 i = 0; i < s_lcd_stream.buffered_count; ++i)
                {
                    LcdChunk& chunk = s_lcd_stream.buffered_chunks[i];
                    if (chunk.valid && chunk.y_start == s_lcd_stream.next_y)
                    {
                        s_lcd->drawRgb565Chunk(chunk.data, width, chunk.y_start, chunk.height_chunk);
                        s_lcd_stream.next_y = chunk.y_start + chunk.height_chunk;
                        ESP_LOGI(TAG, "STREAM buffered chunk drawn: y=%u-%u, next_y=%u",
                                 chunk.y_start, chunk.y_start + chunk.height_chunk - 1, s_lcd_stream.next_y);
                        
                        // Освобождаем память и удаляем из буфера
                        heap_caps_free(chunk.data);
                        chunk.valid = false;
                        
                        // Сдвигаем массив
                        for (u16 j = i; j < s_lcd_stream.buffered_count - 1; ++j)
                        {
                            s_lcd_stream.buffered_chunks[j] = s_lcd_stream.buffered_chunks[j + 1];
                        }
                        s_lcd_stream.buffered_count--;
                        found_next = true;
                        break;
                    }
                }
                if (!found_next)
                {
                    // Больше нет подходящих чанков в буфере
                    break;
                }
            }
            
            // Если после вывода буферизованных чанков всё ещё есть буферизованные,
            // но они не подходят по порядку - логируем это
            if (s_lcd_stream.buffered_count > 0)
            {
                ESP_LOGI(TAG, "STREAM: %u chunks still buffered (waiting for y=%u)",
                         s_lcd_stream.buffered_count, s_lcd_stream.next_y);
            }
        }
        else
        {
            // Чанк не по порядку - буферизуем
                if (s_lcd_stream.buffered_count < LcdStreamState::MAX_BUFFERED_CHUNKS)
                {
                    LcdChunk& chunk = s_lcd_stream.buffered_chunks[s_lcd_stream.buffered_count];
                    chunk.data = (u8*)heap_caps_malloc(chunk_data_len, MALLOC_CAP_8BIT);
                if (chunk.data)
                {
                    memcpy(chunk.data, chunk_data, chunk_data_len);
                    chunk.data_len = chunk_data_len;
                    chunk.y_start = y_start;
                    chunk.height_chunk = height_chunk;
                    chunk.valid = true;
                    s_lcd_stream.buffered_count++;
                    ESP_LOGI(TAG, "STREAM chunk buffered: y=%u-%u (expected y=%u), buffered=%u",
                             y_start, y_start + height_chunk - 1, s_lcd_stream.next_y, s_lcd_stream.buffered_count);
                    
                    // Отправляем подтверждение приёма (даже если буферизован)
                    char ack_buf[32];
                    int ack_len = snprintf(ack_buf, sizeof(ack_buf), "CHUNK_ACK:%u", chunk_index);
                    if (ack_len > 0 && ack_len < (int)sizeof(ack_buf))
                    {
                        msg_header_t ack_header = msg_create_header(
                            MSG_TYPE_RESPONSE,
                            MSG_SRC_ESP32,
                            MSG_DST_EXTERNAL,
                            128,
                            stream_id,
                            (u32)ack_len,
                            sequence,
                            MSG_ROUTE_NONE
                        );
                        
                        if (g_message_bridge &&
                            g_message_bridge->send_message(ack_header, 
                                                           reinterpret_cast<const uint8_t*>(ack_buf), 
                                                           (uint32_t)ack_len))
                        {
                            ESP_LOGI(TAG, "STREAM chunk ACK sent (buffered): chunk=%u, seq=%u", chunk_index, sequence);
                        }
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to allocate buffer for chunk y=%u", y_start);
                }
            }
            else
            {
                ESP_LOGW(TAG, "Chunk buffer full, dropping chunk y=%u", y_start);
            }
        }
    }
    else
    {
        ESP_LOGE(TAG, "LCD not initialized or chunk data empty");
    }

    s_lcd_stream.received_chunks++;

    // Проверка завершения кадра
    // Кадр завершён только когда ВСЕ три условия выполнены одновременно:
    bool all_chunks_received = (s_lcd_stream.received_chunks >= s_lcd_stream.total_chunks);
    bool all_chunks_drawn = (s_lcd_stream.next_y >= s_lcd_stream.height);
    bool buffer_empty = (s_lcd_stream.buffered_count == 0);
    
    ESP_LOGI(TAG, "STREAM progress: received=%u/%u, next_y=%u/%u, buffered=%u",
             s_lcd_stream.received_chunks, s_lcd_stream.total_chunks,
             s_lcd_stream.next_y, s_lcd_stream.height,
             s_lcd_stream.buffered_count);
    
    if (all_chunks_received && all_chunks_drawn && buffer_empty)
    {
        ESP_LOGI(TAG, "STREAM frame complete (all chunks received, drawn, and buffer empty)");

        // Отправляем подтверждение клиенту
        const char* ok_text = "LCD_FRAME_OK";
        msg_header_t response_header = msg_create_header(
            MSG_TYPE_RESPONSE,
            MSG_SRC_ESP32,
            MSG_DST_EXTERNAL,
            128,
            stream_id,
            (u32)strlen(ok_text),
            sequence,
            MSG_ROUTE_NONE
        );

        if (g_message_bridge &&
            g_message_bridge->send_message(
                response_header,
                reinterpret_cast<const u8*>(ok_text),
                (u32)strlen(ok_text)))
        {
            ESP_LOGI(TAG, "LCD frame response sent");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to send LCD frame response");
        }

        // Освобождаем буферизованные чанки (если остались)
        for (u16 i = 0; i < s_lcd_stream.buffered_count; ++i)
        {
            if (s_lcd_stream.buffered_chunks[i].valid && s_lcd_stream.buffered_chunks[i].data)
            {
                heap_caps_free(s_lcd_stream.buffered_chunks[i].data);
            }
        }
        
        // Сброс состояния
        s_lcd_stream = LcdStreamState();
    }
}

void stream_handler_cleanup() {
    // Освобождаем все буферизованные чанки
    for (u16 i = 0; i < s_lcd_stream.buffered_count; ++i)
    {
        if (s_lcd_stream.buffered_chunks[i].valid && s_lcd_stream.buffered_chunks[i].data)
        {
            heap_caps_free(s_lcd_stream.buffered_chunks[i].data);
            s_lcd_stream.buffered_chunks[i].valid = false;
        }
    }
    s_lcd_stream = LcdStreamState();
}

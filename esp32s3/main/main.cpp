#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "ili9486_display.hpp"

// Режим отладки дисплея:
// если определить этот макрос (например, через CMake или просто раскомментировать строку),
// стек Wi‑Fi и TCP клиент НЕ будут запускаться, останется только работа дисплея.
// #define LCD_DEBUG_NO_NET

#include "esp_lcd_panel_io.h"
#include "i80_lcd_bus.hpp"

#include "netpack.h"
#include "wifi.hpp"
#include "tcp.hpp"
#include "message_bridge.hpp"
#include "uart_bridge.hpp"
#include "message_protocol.h"

static const char* TAG = "vkorobka";

// Настройки Wi-Fi
#define WIFI_SSID "ModelSim"
#define WIFI_PASS "adminadmin"

// Настройки TCP
#define SERVER_IP   "192.168.43.236"
#define SERVER_PORT 1234

// Объекты дисплея
static Ili9486Display* s_lcd = nullptr;

// Настройки потоковой передачи кадров на дисплей
static constexpr u16 LCD_STREAM_ID_FRAME = 1;

// Структура для хранения одного чанка в буфере
struct LcdChunk
{
    u8*  data;
    u32  data_len;
    u16  y_start;
    u16  height_chunk;
    bool valid;
};

// Состояние потокового вывода кадра на дисплей (с буферизацией для переупорядочивания)
struct LcdStreamState
{
    u16  width         = 0;
    u16  height        = 0;
    u16  total_chunks  = 0;
    u16  received_chunks = 0;
    u16  format_code   = 0;
    u16  next_y        = 0;  // Следующая ожидаемая Y-координата для вывода
    bool active        = false;
    
    // Буфер для переупорядочивания чанков (максимум 32 чанка)
    static constexpr u16 MAX_BUFFERED_CHUNKS = 32;
    LcdChunk buffered_chunks[MAX_BUFFERED_CHUNKS];
    u16 buffered_count = 0;
};

static LcdStreamState s_lcd_stream;

// Нет отдельной задачи отображения — тестовый паттерн рисуется один раз из app_main.

// Глобальные сетевые объекты
static wifi_t* g_wifi = nullptr;
static tcp_t* g_tcp = nullptr;
message_bridge_t* g_message_bridge = nullptr;  // Убрано static для доступа из tcp.cpp

// Обработчик новых сообщений через message_bridge
static void handle_new_message(const msg_header_t* header, const u8* payload, u32 payload_len)
{
    if (header == nullptr)
    {
        return;
    }
    
    ESP_LOGI(TAG, "[RX] New protocol message: type=%d, src=%d, dst=%d, len=%u",
             header->msg_type, header->source_id, header->destination_id, header->payload_len);
    
    // Обработка команд TEST и STATUS от win-x64
    if (header->source_id == MSG_SRC_WIN && header->destination_id == MSG_DST_ESP32 && 
        header->msg_type == MSG_TYPE_COMMAND && payload && payload_len > 0)
    {
        // Создаем временную строку для сравнения
        char* str_buf = (char*)malloc(payload_len + 1);
        if (str_buf) {
            memcpy(str_buf, payload, payload_len);
            str_buf[payload_len] = '\0';
            
            // Обработка команды TEST
            if (strcmp(str_buf, "TEST") == 0) {
                ESP_LOGI(TAG, "Received TEST command, sending response");
                free(str_buf);
                
                // Отправляем ответ через новый протокол
                const char* response = "TEST_RESPONSE";
                msg_header_t response_header = msg_create_header(
                    MSG_TYPE_RESPONSE,
                    MSG_SRC_ESP32,
                    MSG_DST_WIN,
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
                    ESP_LOGI(TAG, "Test response sent successfully via new protocol");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to send test response via new protocol");
                }
                return;
            }
            
            // Обработка команды STATUS
            if (strcmp(str_buf, "STATUS") == 0) {
                ESP_LOGI(TAG, "Received STATUS command, sending status");
                free(str_buf);
                
                // Формируем строку статуса: STATUS:WIFI=<state>:<connected>,TCP=<state>:<connected>
                char status_buf[256];
                const char* wifi_state_str = "UNKNOWN";
                bool wifi_connected = false;
                const char* tcp_state_str = "UNKNOWN";
                bool tcp_connected = false;
                
                if (g_wifi) {
                    wifi_connected = g_wifi->is_connected();
                    // На ESP32 WiFi не имеет состояний как на win-x64, только подключен/не подключен
                    wifi_state_str = wifi_connected ? "CONNECTED" : "DISCONNECTED";
                }
                
                if (g_tcp) {
                    tcp_state_t tcp_state = g_tcp->get_state();
                    tcp_connected = g_tcp->is_connected();
                    
                    // На ESP32 tcp_state_t - это enum, а не enum class
                    switch (tcp_state) {
                        case TCP_STATE_INIT: tcp_state_str = "INIT"; break;
                        case TCP_STATE_CONNECTING: tcp_state_str = "CONNECTING"; break;
                        case TCP_STATE_CONNECTED: tcp_state_str = "CONNECTED"; break;
                        case TCP_STATE_LOST: tcp_state_str = "LOST"; break;
                        case TCP_STATE_RETRY: tcp_state_str = "RETRY"; break;
                        default: tcp_state_str = "UNKNOWN"; break;
                    }
                }
                
                int status_len = snprintf(status_buf, sizeof(status_buf),
                            "STATUS:WIFI=%s:%d,TCP=%s:%d",
                            wifi_state_str, wifi_connected ? 1 : 0,
                            tcp_state_str, tcp_connected ? 1 : 0);
                
                if (status_len > 0 && status_len < (int)sizeof(status_buf)) {
                    // Отправляем ответ через новый протокол
                    msg_header_t response_header = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_WIN,
                        128,
                        0,
                        status_len,
                        0,
                        MSG_ROUTE_NONE
                    );
                    
                    if (g_message_bridge && g_message_bridge->send_message(response_header, 
                                                                            reinterpret_cast<const u8*>(status_buf), 
                                                                            status_len))
                    {
                        ESP_LOGI(TAG, "Status response sent successfully via new protocol");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to send status response via new protocol");
                    }
                }
                return;
            }
            
            free(str_buf);
        }
    }
    
    // Обработка тестовых сообщений с изображениями для ESP32 (старый путь JPG-эхо)
    if (header->destination_id == MSG_DST_ESP32 && header->msg_type == MSG_TYPE_DATA && payload && payload_len > 0)
    {
        ESP_LOGI(TAG, "[ESP32] Processing image test for ESP32 component");
        
        // Временное решение: возвращаем исходное изображение без изменений
        // Простое переворачивание байтов портит JPG формат
        // В реальной реализации здесь должна быть декомпрессия JPG, отзеркаливание и рекомпрессия
        std::vector<u8> mirrored_payload(payload, payload + payload_len);
        
        ESP_LOGI(TAG, "[ESP32] Image returned (original, not mirrored): %u bytes", payload_len);
        
        // Формируем ответ
        msg_header_t response_header = msg_create_header(
            MSG_TYPE_RESPONSE,
            MSG_SRC_ESP32,
            MSG_DST_EXTERNAL,
            128,
            0,
            payload_len,
            0,
            MSG_ROUTE_NONE
        );
        
        // Отправляем ответ обратно через TCP используя message_bridge
        if (g_message_bridge)
        {
            if (g_message_bridge->send_message(response_header, mirrored_payload.data(), payload_len))
            {
                ESP_LOGI(TAG, "[ESP32] Response sent to Windows via message_bridge");
            }
            else
            {
                ESP_LOGE(TAG, "[ESP32] Failed to send response via message_bridge");
            }
        }
        else
        {
            ESP_LOGW(TAG, "[ESP32] message_bridge not available, cannot send response");
        }
    }

    // Обработка потоковых кадров для дисплея (STREAM с RGB565 кадром из чанков)
    // Реализация потокового вывода: чанки выводятся сразу на дисплей без накопления в памяти
    if (header->destination_id == MSG_DST_ESP32 &&
        header->msg_type == MSG_TYPE_STREAM &&
        header->stream_id == LCD_STREAM_ID_FRAME &&
        payload && payload_len > 0)
    {
        ESP_LOGI(TAG, "[ESP32] STREAM frame chunk: len=%u", payload_len);

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
            ESP_LOGE(TAG, "[ESP32] STREAM chunk too small: %u (header_size=%u)", payload_len, header_size);
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
                 "[ESP32] STREAM hdr: %ux%u, chunk=%u/%u, fmt=%u, y_start=%u, h_chunk=%u, data_len=%u",
                 width, height, chunk_index, total_chunks, format_code, y_start, height_chunk, chunk_data_len);

        if (width == 0 || height == 0 || total_chunks == 0 || chunk_index >= total_chunks)
        {
            ESP_LOGE(TAG, "[ESP32] STREAM header invalid, drop chunk");
            return;
        }

        // Поддерживаем только формат 1 = RGB565_BE
        if (format_code != 1)
        {
            ESP_LOGE(TAG, "[ESP32] Unsupported frame format_code=%u", format_code);
            return;
        }

        // Инициализация состояния при первом чанке
        if (!s_lcd_stream.active)
        {
            ESP_LOGI(TAG, "[ESP32] STREAM start new frame (streaming mode with buffering)");
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
            ESP_LOGE(TAG, "[ESP32] STREAM header mismatch with active frame, resetting state");
            s_lcd_stream = LcdStreamState();
            return;
        }

        // Буферизация и вывод чанков с переупорядочиванием
        if (s_lcd && chunk_data_len > 0)
        {
            const u8* chunk_data = payload + header_size;
            
            // Если чанк идёт по порядку - выводим сразу
            if (y_start == s_lcd_stream.next_y)
            {
                s_lcd->drawRgb565Chunk(chunk_data, width, y_start, height_chunk);
                s_lcd_stream.next_y = y_start + height_chunk;
                ESP_LOGI(TAG, "[ESP32] STREAM chunk drawn immediately: y=%u-%u, next_y=%u", 
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
                        header->stream_id,
                        (u32)ack_len,
                        header->sequence,  // Используем тот же sequence номер
                        MSG_ROUTE_NONE
                    );
                    
                    if (g_message_bridge &&
                        g_message_bridge->send_message(ack_header, 
                                                       reinterpret_cast<const u8*>(ack_buf), 
                                                       (u32)ack_len))
                    {
                        ESP_LOGI(TAG, "[ESP32] STREAM chunk ACK sent: chunk=%u, seq=%u", chunk_index, header->sequence);
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
                            ESP_LOGI(TAG, "[ESP32] STREAM buffered chunk drawn: y=%u-%u, next_y=%u",
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
                    ESP_LOGI(TAG, "[ESP32] STREAM: %u chunks still buffered (waiting for y=%u)",
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
                        ESP_LOGI(TAG, "[ESP32] STREAM chunk buffered: y=%u-%u (expected y=%u), buffered=%u",
                                 y_start, y_start + height_chunk - 1, s_lcd_stream.next_y, s_lcd_stream.buffered_count);
                        
                        // Отправляем подтверждение приёма (даже если буферизован)
                        // Это важно для flow control - отправитель должен знать, что чанк принят
                        char ack_buf[32];
                        int ack_len = snprintf(ack_buf, sizeof(ack_buf), "CHUNK_ACK:%u", chunk_index);
                        if (ack_len > 0 && ack_len < (int)sizeof(ack_buf))
                        {
                            msg_header_t ack_header = msg_create_header(
                                MSG_TYPE_RESPONSE,
                                MSG_SRC_ESP32,
                                MSG_DST_EXTERNAL,
                                128,
                                header->stream_id,
                                (u32)ack_len,
                                header->sequence,
                                MSG_ROUTE_NONE
                            );
                            
                            if (g_message_bridge &&
                                g_message_bridge->send_message(ack_header, 
                                                               reinterpret_cast<const u8*>(ack_buf), 
                                                               (u32)ack_len))
                            {
                                ESP_LOGI(TAG, "[ESP32] STREAM chunk ACK sent (buffered): chunk=%u, seq=%u", chunk_index, header->sequence);
                            }
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "[ESP32] Failed to allocate buffer for chunk y=%u", y_start);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "[ESP32] Chunk buffer full, dropping chunk y=%u", y_start);
                }
            }
        }
        else
        {
            ESP_LOGE(TAG, "[ESP32] LCD not initialized or chunk data empty");
        }

        s_lcd_stream.received_chunks++;

        // Проверяем завершение кадра
        // Кадр завершён только когда:
        // 1. Все чанки получены (received_chunks >= total_chunks)
        // 2. Все чанки выведены (next_y >= height)
        // 3. Буфер пуст (buffered_count == 0) - все буферизованные чанки выведены
        bool all_chunks_received = (s_lcd_stream.received_chunks >= s_lcd_stream.total_chunks);
        bool all_chunks_drawn = (s_lcd_stream.next_y >= s_lcd_stream.height);
        bool buffer_empty = (s_lcd_stream.buffered_count == 0);
        
        ESP_LOGI(TAG, "[ESP32] STREAM progress: received=%u/%u, next_y=%u/%u, buffered=%u",
                 s_lcd_stream.received_chunks, s_lcd_stream.total_chunks,
                 s_lcd_stream.next_y, s_lcd_stream.height,
                 s_lcd_stream.buffered_count);
        
        if (all_chunks_received && all_chunks_drawn && buffer_empty)
        {
            ESP_LOGI(TAG, "[ESP32] STREAM frame complete (all chunks received, drawn, and buffer empty)");

            // Отправляем подтверждение клиенту
            const char* ok_text = "LCD_FRAME_OK";
            msg_header_t response_header = msg_create_header(
                MSG_TYPE_RESPONSE,
                MSG_SRC_ESP32,
                MSG_DST_EXTERNAL,
                128,
                header->stream_id,
                (u32)strlen(ok_text),
                header->sequence,
                MSG_ROUTE_NONE
            );

            if (g_message_bridge &&
                g_message_bridge->send_message(
                    response_header,
                    reinterpret_cast<const u8*>(ok_text),
                    (u32)strlen(ok_text)))
            {
                ESP_LOGI(TAG, "[ESP32] LCD frame response sent");
            }
            else
            {
                ESP_LOGE(TAG, "[ESP32] Failed to send LCD frame response");
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
}

// Обработчик принятых пакетов (старый формат - больше не используется)
static void handle_packet(cmdcode_t cmd_code, const char* payload, u32 payload_len) {
    // Старый CMD протокол больше не используется
    // Все сообщения должны приходить через новый протокол
    ESP_LOGW(TAG, "[RX] Legacy CMD packet received (ignored): CMD=0x%04x, len=%u", cmd_code, payload_len);
    ESP_LOGW(TAG, "Note: All communication should use the new message_protocol");
    
    // Пытаемся обработать как новый протокол (fallback)
        if (g_message_bridge && payload_len >= MSG_HEADER_LEN)
        {
            const u8* buffer = reinterpret_cast<const u8*>(payload);
            if (g_message_bridge->process_buffer(buffer, payload_len))
            {
                // Успешно обработано как новое сообщение
                return;
            }
        }
    
    // Старая обработка команд больше не используется
    if (false && cmd_code == CMD_WIN && payload_len > 0 && payload != nullptr) {
        // Создаем временную строку для сравнения
        char* str_buf = (char*)malloc(payload_len + 1);
        if (str_buf) {
            memcpy(str_buf, payload, payload_len);
            str_buf[payload_len] = '\0';
            
            // Обработка команды TEST
            if (strcmp(str_buf, "TEST") == 0) {
                ESP_LOGI(TAG, "Received TEST command, sending response");
                if (g_tcp && g_tcp->is_connected()) {
                    if (g_tcp->send_packet_str(CMD_ESP, "TEST_RESPONSE")) {
                        ESP_LOGI(TAG, "Test response sent successfully");
                    } else {
                        ESP_LOGE(TAG, "Failed to send test response");
                    }
                } else {
                    ESP_LOGW(TAG, "TCP not connected, cannot send test response");
                }
                free(str_buf);
                return;
            }
            
            // Обработка команды STATUS
            if (strcmp(str_buf, "STATUS") == 0) {
                ESP_LOGI(TAG, "Received STATUS command, sending status");
                if (g_tcp && g_tcp->is_connected()) {
                    // Формируем строку статуса: STATUS:WIFI=<state>:<connected>,TCP=<state>:<connected>
                    char status_buf[256];
                    const char* wifi_state_str = "UNKNOWN";
                    int wifi_connected = 0;
                    
                    if (g_wifi) {
                        wifi_connected = g_wifi->is_connected() ? 1 : 0;
                        // WiFi на ESP32 не имеет состояний как на win-x64, только подключен/не подключен
                        wifi_state_str = wifi_connected ? "CONNECTED" : "DISCONNECTED";
                    }
                    
                    const char* tcp_state_str = "UNKNOWN";
                    int tcp_connected = 0;
                    
                    if (g_tcp) {
                        tcp_state_str = g_tcp->get_state_string();
                        tcp_connected = g_tcp->is_connected() ? 1 : 0;
                    }
                    
                    snprintf(status_buf, sizeof(status_buf), 
                            "STATUS:WIFI=%s:%d,TCP=%s:%d",
                            wifi_state_str, wifi_connected,
                            tcp_state_str, tcp_connected);
                    
                    if (g_tcp->send_packet_str(CMD_ESP, status_buf)) {
                        ESP_LOGI(TAG, "Status sent successfully: %s", status_buf);
                    } else {
                        ESP_LOGE(TAG, "Failed to send status");
                    }
                } else {
                    ESP_LOGW(TAG, "TCP not connected, cannot send status");
                }
                free(str_buf);
                return;
            }
            
            // Обычный вывод для других команд
            ESP_LOGI(TAG, "PC: %s", str_buf);
            free(str_buf);
        }
    }
    
    // Обычный вывод для других типов пакетов
    if (payload_len > 0 && payload != nullptr) {
        // Создаем временную строку для вывода
        char* str_buf = (char*)malloc(payload_len + 1);
        if (str_buf) {
            memcpy(str_buf, payload, payload_len);
            str_buf[payload_len] = '\0';
            ESP_LOGI(TAG, "PC: %s", str_buf);
            free(str_buf);
        }
    }
    
    switch (cmd_code) {
        case CMD_WIN:
            ESP_LOGI(TAG, "  Type: Windows command");
            break;
        case CMD_ESP:
            ESP_LOGI(TAG, "  Type: ESP32 command");
            break;
        case CMD_STM:
            ESP_LOGI(TAG, "  Type: STM32 command");
            break;
        case CMD_DPL:
            ESP_LOGI(TAG, "  Type: Display data");
            break;
        case CMD_MIC:
            ESP_LOGI(TAG, "  Type: Microphone data");
            break;
        case CMD_SPK:
            ESP_LOGI(TAG, "  Type: Speaker data");
            break;
        default:
            ESP_LOGW(TAG, "  Type: Unknown");
            break;
    }
}

// Задача для чтения консольного ввода и отправки сообщений
static void console_input_task(void* pvParameters) {
    char payload[256];
    int pos = 0;
    
    // Ждем, пока TCP будет инициализирован
    while (g_tcp == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "Console input task ready");
    printf("Type message and press Enter to send:\r\n");
    printf("  Format: <data> - send raw data\r\n");
    printf("  Format: send <code> <data> - send CMD packet (code: 0x41 or 65)\r\n");
    fflush(stdout);
    
    while (1) {
        // Используем fread для неблокирующего чтения
        int ch = getchar();
        
        if (ch == EOF || ch == -1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                payload[pos] = '\0';
                
                if (g_tcp && g_tcp->is_connected()) {
                    // Обработка команды status
                    if (strcmp(payload, "status") == 0) {
                        ESP_LOGI(TAG, "=== Status Information ===");
                        
                        // WiFi Status
                        ESP_LOGI(TAG, "WiFi:");
                        if (g_wifi) {
                            bool wifi_connected = g_wifi->is_connected();
                            ESP_LOGI(TAG, "  Connected: %s", wifi_connected ? "YES" : "NO");
                        } else {
                            ESP_LOGE(TAG, "  Status: NOT INITIALIZED");
                        }
                        
                        // TCP Status
                        ESP_LOGI(TAG, "TCP:");
                        if (g_tcp) {
                            const char* tcp_state_str = g_tcp->get_state_string();
                            bool tcp_connected = g_tcp->is_connected();
                            ESP_LOGI(TAG, "  State: %s", tcp_state_str);
                            ESP_LOGI(TAG, "  Connected: %s", tcp_connected ? "YES" : "NO");
                        } else {
                            ESP_LOGE(TAG, "  Status: NOT INITIALIZED");
                        }
                        
                        printf("\r\n");
                        pos = 0;
                        continue;
                    }
                    
                    // Проверяем, является ли это командой send с кодом
                    // Формат: "send <code> <data>" или просто "<data>"
                    if (strncmp(payload, "send ", 5) == 0) {
                        // Парсим команду send
                        char* cmd_ptr = payload + 5;  // Пропускаем "send "
                        
                        // Пытаемся найти код команды (может быть hex 0x41 или decimal 65)
                        cmdcode_t cmd_code = CMD_RESERVED;
                        char* code_end = NULL;
                        
                        // Пробуем распарсить как hex (0x...)
                        if (cmd_ptr[0] == '0' && (cmd_ptr[1] == 'x' || cmd_ptr[1] == 'X')) {
                            cmd_code = (cmdcode_t)strtoul(cmd_ptr, &code_end, 16);
                        } else {
                            // Пробуем распарсить как decimal
                            cmd_code = (cmdcode_t)strtoul(cmd_ptr, &code_end, 10);
                        }
                        
                        // Если удалось распарсить код и есть данные после него
                        if (code_end != NULL && code_end != cmd_ptr && *code_end == ' ' && cmd_code != CMD_RESERVED) {
                            // Пропускаем пробел и берем данные
                            char* data_ptr = code_end + 1;
                            // Вычисляем длину данных как разницу между концом буфера и началом данных
                            // Используем pos (фактическая длина) вместо strlen() для корректной обработки нулевых байт
                            int data_start_offset = (int)(data_ptr - payload);
                            int data_len = pos - data_start_offset;
                            
                            if (data_len > 0) {
                                // Отправляем в формате CMD
                                if (g_tcp->send_packet(cmd_code, data_ptr, data_len)) {
                                    ESP_LOGI(TAG, "Sent packet: CMD=0x%04x, Data length=%d bytes", cmd_code, data_len);
                                } else {
                                    ESP_LOGE(TAG, "Failed to send packet");
                                }
                            } else {
                                ESP_LOGW(TAG, "No data to send. Usage: send <code> <data>");
                            }
                        } else {
                            // Если не удалось распарсить код, отправляем как raw данные
                            // Используем фактическую длину от cmd_ptr до конца буфера
                            int cmd_start_offset = (int)(cmd_ptr - payload);
                            int cmd_data_len = pos - cmd_start_offset;
                            if (cmd_data_len > 0 && g_tcp->send(cmd_ptr, cmd_data_len)) {
                                ESP_LOGI(TAG, "Sent %d bytes (raw)", cmd_data_len);
                            } else {
                                ESP_LOGE(TAG, "Failed to send message");
                            }
                        }
                    } else {
                        // Отправляем как raw данные (как было в оригинале)
                        if (g_tcp->send(payload, pos)) {
                            ESP_LOGI(TAG, "Sent %d bytes: %s", pos, payload);
                        } else {
                            ESP_LOGE(TAG, "Failed to send message");
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "Not connected, cannot send message");
                }
                
                printf("\r\n");
                pos = 0;
            }
        }
        else if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
        }
        else if (ch >= 32 && ch <= 126 && pos < sizeof(payload) - 1) {
            payload[pos++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "vkorobka: App starting...");
    
    // Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing... ret = 0x%x", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");

    // Инициализация дисплея (аппаратный I80 через esp_lcd)
    ESP_LOGI(TAG, "LCD: Using HARDWARE I80 bus");
    ESP_LOGI(TAG, "LCD: init I80 bus + IO...");
    esp_lcd_panel_io_handle_t io_handle = init_ili9486_i80_panel_io();

    // Пробуем прочитать несколько регистров до инициализации контроллера
    ESP_LOGI(TAG, "LCD: debug-read registers BEFORE init...");
    i80_lcd_debug_read_id();

    static Ili9486Display lcd(io_handle);
    s_lcd = &lcd;
    ESP_LOGI(TAG, "LCD: init controller...");
    lcd.init();

    // И ещё раз после инициализации, чтобы увидеть, изменились ли значения
    ESP_LOGI(TAG, "LCD: debug-read registers AFTER init...");
    i80_lcd_debug_read_id();

    // Рисуем тестовый паттерн: цветовые градиенты + градации серого
    ESP_LOGI(TAG, "LCD: draw test pattern...");
    lcd.drawTestPattern();

    // Инициализация сетевого стека (может быть отключена в режиме отладки дисплея)
#ifndef LCD_DEBUG_NO_NET
    // Инициализация сетевого стека
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    
    // Инициализация Wi-Fi
    g_wifi = new wifi_t();
    if (!g_wifi->init(WIFI_SSID, WIFI_PASS)) {
        ESP_LOGE(TAG, "WiFi initialization failed");
        return;
    }
    g_wifi->wait_for_connection();
    ESP_LOGI(TAG, "Wi-Fi connected");
    
    // Инициализация message bridge
    g_message_bridge = new message_bridge_t();
    g_message_bridge->init(nullptr);  // TCP будет установлен позже
    g_message_bridge->register_handler(MSG_TYPE_COMMAND, handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_DATA, handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_STREAM, handle_new_message);
    
    // Инициализация TCP
    g_tcp = new tcp_t(SERVER_IP, SERVER_PORT, g_wifi);
    g_tcp->set_packet_callback(handle_packet);
    
    // Устанавливаем TCP в message_bridge
    g_message_bridge->init(g_tcp);
    
    if (!g_tcp->start()) {
        ESP_LOGE(TAG, "TCP client start failed");
        return;
    }
    ESP_LOGI(TAG, "TCP client started");
    
    // Запускаем задачу для чтения консольного ввода
    xTaskCreate(console_input_task, "console_input", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Console input task created");
    
    // Основной цикл
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    ESP_LOGW(TAG, "LCD DEBUG MODE: Wi-Fi/TCP stack NOT started (LCD_DEBUG_NO_NET)");
    // В режиме отладки дисплея просто остаёмся в бесконечном цикле,
    // чтобы задача app_main не завершилась сразу.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

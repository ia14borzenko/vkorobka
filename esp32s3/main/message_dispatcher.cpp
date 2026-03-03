#include "message_dispatcher.hpp"
#include "command_handler.hpp"
#include "stream_handler.hpp"
#include "message_bridge.hpp"
#include "esp_log.h"
#include "my_types.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "message_dispatcher";

// Внешние ссылки на глобальные объекты
extern Ili9486Display* s_lcd;
extern message_bridge_t* g_message_bridge;

// Обработчик новых сообщений через message_bridge
void message_dispatcher_handle_new_message(const msg_header_t* header, const u8* payload, u32 payload_len)
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
                free(str_buf);
                command_handle_test();
                return;
            }
            
            // Обработка команды STATUS
            if (strcmp(str_buf, "STATUS") == 0) {
                free(str_buf);
                command_handle_status();
                return;
            }
            
            free(str_buf);
        }
    }
    
    // Обработка команд текстинга от pyapp (EXTERNAL source)
    if (header->source_id == MSG_SRC_EXTERNAL && header->destination_id == MSG_DST_ESP32 && 
        header->msg_type == MSG_TYPE_COMMAND && payload && payload_len > 0 && s_lcd)
    {
        // Создаем временную строку для парсинга JSON
        char* json_buf = (char*)malloc(payload_len + 1);
        if (!json_buf) {
            ESP_LOGE(TAG, "[TEXTING] Failed to allocate memory for JSON parsing");
            return;
        }
        
        memcpy(json_buf, payload, payload_len);
        json_buf[payload_len] = '\0';
        
        ESP_LOGI(TAG, "[TEXTING] Received command from EXTERNAL, payload_len=%u", payload_len);
        ESP_LOGI(TAG, "[TEXTING] Payload (first 200 chars): %.200s", json_buf);
        
        command_handle_texting(json_buf, payload_len);
        
        free(json_buf);
        return;
    }
    
    // Обработка тестовых сообщений с изображениями для ESP32 (старый путь JPG-эхо)
    if (header->destination_id == MSG_DST_ESP32 && header->msg_type == MSG_TYPE_DATA && payload && payload_len > 0)
    {
        command_handle_image_data(payload, payload_len);
        return;
    }

    // Обработка потоковых кадров для дисплея (STREAM с RGB565 кадром из чанков)
    if (header->destination_id == MSG_DST_ESP32 &&
        header->msg_type == MSG_TYPE_STREAM &&
        payload && payload_len > 0)
    {
        stream_handler_process_chunk(payload, payload_len, header->stream_id, header->sequence);
        return;
    }
}

// Обработчик принятых пакетов (старый формат - больше не используется)
void message_dispatcher_handle_packet(cmdcode_t cmd_code, const char* payload, u32 payload_len) {
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
}

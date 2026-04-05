#include "message_dispatcher.hpp"
#include "command_handler.hpp"
#include "stream_handler.hpp"
#include "mic_stream.hpp"
#include "dyn_playback.hpp"
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
    
    if (header->msg_type == MSG_TYPE_STREAM)
    {
        ESP_LOGD(TAG, "[RX] STREAM src=%d dst=%d stream_id=%u len=%u", header->source_id,
                 header->destination_id, (unsigned)header->stream_id, header->payload_len);
    }
    else
    {
        ESP_LOGI(TAG, "[RX] New protocol message: type=%d, src=%d, dst=%d, len=%u",
                 header->msg_type, header->source_id, header->destination_id, header->payload_len);
    }
    
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
    
    // voice.on / voice.off — до TEXTING (payload текстовый, не JSON)
    if (header->source_id == MSG_SRC_EXTERNAL && header->destination_id == MSG_DST_ESP32 &&
        header->msg_type == MSG_TYPE_COMMAND && payload && payload_len > 0)
    {
        char* vbuf = (char*)malloc(payload_len + 1);
        if (vbuf)
        {
            memcpy(vbuf, payload, payload_len);
            vbuf[payload_len] = '\0';
            if (strncmp(vbuf, "voice.set", 9) == 0 &&
                (vbuf[9] == '\0' || vbuf[9] == ' ' || vbuf[9] == '\t' || vbuf[9] == '{'))
            {
                const char* p = vbuf + 9;
                while (*p == ' ' || *p == '\t')
                {
                    ++p;
                }
                const bool ok = (*p == '{') && mic_stream_set_config_json(p);
                free(vbuf);
                if (g_message_bridge)
                {
                    const char* ack = ok ? "VOICE_SET_OK" : "VOICE_SET_ERR";
                    msg_header_t rh = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_EXTERNAL,
                        128,
                        0,
                        (u32)strlen(ack),
                        0,
                        MSG_ROUTE_NONE);
                    g_message_bridge->send_message(rh, reinterpret_cast<const u8*>(ack), (u32)strlen(ack));
                }
                return;
            }
            if (strncmp(vbuf, "dyn.set", 7) == 0 &&
                (vbuf[7] == '\0' || vbuf[7] == ' ' || vbuf[7] == '\t' || vbuf[7] == '{'))
            {
                const char* p = vbuf + 7;
                while (*p == ' ' || *p == '\t')
                {
                    ++p;
                }
                const bool ok = (*p == '{') && dyn_playback_set_config_json(p);
                free(vbuf);
                if (g_message_bridge)
                {
                    const char* ack = ok ? "DYN_SET_OK" : "DYN_SET_ERR";
                    msg_header_t rh = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_EXTERNAL,
                        128,
                        0,
                        (u32)strlen(ack),
                        0,
                        MSG_ROUTE_NONE);
                    g_message_bridge->send_message(rh, reinterpret_cast<const u8*>(ack), (u32)strlen(ack));
                }
                return;
            }
            if (strcmp(vbuf, "voice.on") == 0)
            {
                free(vbuf);
                mic_stream_set_tx_enabled(true);
                if (g_message_bridge)
                {
                    const char* ack = "VOICE_ON_OK";
                    msg_header_t rh = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_EXTERNAL,
                        128,
                        0,
                        (u32)strlen(ack),
                        0,
                        MSG_ROUTE_NONE);
                    g_message_bridge->send_message(rh, reinterpret_cast<const u8*>(ack), (u32)strlen(ack));
                }
                return;
            }
            if (strcmp(vbuf, "voice.off") == 0)
            {
                free(vbuf);
                mic_stream_set_tx_enabled(false);
                if (g_message_bridge)
                {
                    const char* ack = "VOICE_OFF_OK";
                    msg_header_t rh = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_EXTERNAL,
                        128,
                        0,
                        (u32)strlen(ack),
                        0,
                        MSG_ROUTE_NONE);
                    g_message_bridge->send_message(rh, reinterpret_cast<const u8*>(ack), (u32)strlen(ack));
                }
                return;
            }
            if (strcmp(vbuf, "dyn.on") == 0)
            {
                free(vbuf);
                dyn_playback_set_armed(true);
                if (g_message_bridge)
                {
                    const char* ack = "DYN_ON_OK";
                    msg_header_t rh = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_EXTERNAL,
                        128,
                        0,
                        (u32)strlen(ack),
                        0,
                        MSG_ROUTE_NONE);
                    g_message_bridge->send_message(rh, reinterpret_cast<const u8*>(ack), (u32)strlen(ack));
                }
                return;
            }
            if (strcmp(vbuf, "dyn.off") == 0)
            {
                free(vbuf);
                dyn_playback_set_armed(false);
                if (g_message_bridge)
                {
                    const char* ack = "DYN_OFF_OK";
                    msg_header_t rh = msg_create_header(
                        MSG_TYPE_RESPONSE,
                        MSG_SRC_ESP32,
                        MSG_DST_EXTERNAL,
                        128,
                        0,
                        (u32)strlen(ack),
                        0,
                        MSG_ROUTE_NONE);
                    g_message_bridge->send_message(rh, reinterpret_cast<const u8*>(ack), (u32)strlen(ack));
                }
                return;
            }
            free(vbuf);
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

    // Потоковые данные: stream_id 1 = LCD, 3 = PCM на динамик (MAX98357A)
    if (header->destination_id == MSG_DST_ESP32 &&
        header->msg_type == MSG_TYPE_STREAM &&
        payload && payload_len > 0)
    {
        if (header->stream_id == LCD_STREAM_ID_FRAME)
        {
            stream_handler_process_chunk(payload, payload_len, header->stream_id, header->sequence);
            return;
        }
        if (header->stream_id == DYN_STREAM_ID)
        {
            dyn_playback_feed(payload, payload_len);
            return;
        }
        ESP_LOGW(TAG, "Unknown stream_id=%u (not LCD/audio)", (unsigned)header->stream_id);
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

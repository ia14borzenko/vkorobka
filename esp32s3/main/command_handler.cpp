#include "command_handler.hpp"
#include "texting.hpp"
#include "esp_log.h"
#include "message_bridge.hpp"
#include "message_protocol.h"
#include "wifi.hpp"
#include "tcp.hpp"
#include "netpack.h"
#include "my_types.h"
#include <string.h>
#include <stdlib.h>
#include <vector>

static const char* TAG = "command_handler";

// Внешние ссылки на глобальные объекты из main.cpp
extern wifi_t* g_wifi;
extern tcp_t* g_tcp;
extern message_bridge_t* g_message_bridge;

void command_handle_test() {
    ESP_LOGI(TAG, "Received TEST command, sending response");
    
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
}

void command_handle_status() {
    ESP_LOGI(TAG, "Received STATUS command, sending status");
    
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
}

void command_handle_texting(const char* json_buf, u32 json_len) {
    // Простой парсинг JSON для поиска команды
    const char* cmd_clear = strstr(json_buf, "TEXT_CLEAR");
    const char* cmd_add = strstr(json_buf, "TEXT_ADD");
    
    ESP_LOGI(TAG, "Command search: cmd_clear=%p, cmd_add=%p", cmd_clear, cmd_add);
    
    if (cmd_clear) {
        texting_handle_clear(json_buf, json_len);
    } else if (cmd_add) {
        texting_handle_add(json_buf, json_len);
    } else {
        // Команда не распознана
        ESP_LOGW(TAG, "Unknown command in JSON payload");
        ESP_LOGW(TAG, "Full payload: %s", json_buf);
    }
}

void command_handle_image_data(const u8* payload, u32 payload_len) {
    ESP_LOGI(TAG, "Processing image test for ESP32 component");
    
    // Временное решение: возвращаем исходное изображение без изменений
    std::vector<u8> mirrored_payload(payload, payload + payload_len);
    
    ESP_LOGI(TAG, "Image returned (original, not mirrored): %u bytes", payload_len);
    
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
            ESP_LOGI(TAG, "Response sent to Windows via message_bridge");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to send response via message_bridge");
        }
    }
    else
    {
        ESP_LOGW(TAG, "message_bridge not available, cannot send response");
    }
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

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

// Глобальные объекты
static wifi_t* g_wifi = nullptr;
static tcp_t* g_tcp = nullptr;
message_bridge_t* g_message_bridge = nullptr;  // Убрано static для доступа из tcp.cpp
static uart_bridge_t* g_uart_bridge = nullptr;

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
    
    // Обработка тестовых сообщений с изображениями для ESP32
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
        if (g_message_bridge->process_buffer(buffer, payload_len, true))
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
    
    // Инициализация UART bridge
    g_uart_bridge = new uart_bridge_t(UART_NUM_2, 115200);
    if (!g_uart_bridge->init(17, 16))  // TX=17, RX=16 (настройте под вашу плату)
    {
        ESP_LOGE(TAG, "UART bridge initialization failed");
        return;
    }
    if (!g_uart_bridge->start())
    {
        ESP_LOGE(TAG, "UART bridge start failed");
        return;
    }
    ESP_LOGI(TAG, "UART bridge started");

    // Инициализация message bridge
    g_message_bridge = new message_bridge_t();
    g_message_bridge->init(nullptr, g_uart_bridge);  // TCP будет установлен позже
    g_message_bridge->register_handler(MSG_TYPE_COMMAND, handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_DATA, handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_STREAM, handle_new_message);
    
    // Устанавливаем callback для UART
    g_uart_bridge->set_message_callback([](const msg_header_t* header, const u8* payload, u32 payload_len) {
        if (g_message_bridge)
        {
            g_message_bridge->route_from_uart(header, payload, payload_len);
        }
    });
    
    // Инициализация TCP
    g_tcp = new tcp_t(SERVER_IP, SERVER_PORT, g_wifi);
    g_tcp->set_packet_callback(handle_packet);
    
    // Устанавливаем TCP в message_bridge
    g_message_bridge->init(g_tcp, g_uart_bridge);
    
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
}

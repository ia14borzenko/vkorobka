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

// Обработчик принятых пакетов
static void handle_packet(cmdcode_t cmd_code, const char* payload, u32 payload_len) {
    // Простой вывод как строка (как было в оригинале)
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
    
    ESP_LOGI(TAG, "[RX] Packet received: CMD=0x%04x, len=%u", cmd_code, payload_len);
    
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
    
    // Инициализация TCP
    g_tcp = new tcp_t(SERVER_IP, SERVER_PORT, g_wifi);
    g_tcp->set_packet_callback(handle_packet);
    
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

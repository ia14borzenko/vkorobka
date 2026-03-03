#include "console_input.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tcp.hpp"
#include "wifi.hpp"
#include "netpack.h"
#include "my_types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* TAG = "console_input";

extern tcp_t* g_tcp;
extern wifi_t* g_wifi;

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

void console_input_init() {
    xTaskCreate(console_input_task, "console_input", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Console input task created");
}

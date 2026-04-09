#include "console_input.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tcp.hpp"
#include "wifi.hpp"
#include "app_config.hpp"
#include "netpack.h"
#include "my_types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include "lwip/inet.h"

static const char* TAG = "console_input";

extern tcp_t* g_tcp;
extern wifi_t* g_wifi;
extern std::string g_runtime_wifi_ssid;
extern std::string g_runtime_wifi_pass;
extern std::string g_runtime_server_ip;
extern int g_runtime_server_port;

static const char CTRL_T = 0x14;

static const char* auth_mode_to_str(wifi_auth_mode_t mode) {
    switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "UNKNOWN";
    }
}

static void print_line_prompt(const char* prompt, const char* buffer, int pos, bool hidden) {
    printf("\r%s", prompt);
    if (hidden) {
        for (int i = 0; i < pos; ++i) {
            putchar('*');
        }
    } else {
        for (int i = 0; i < pos; ++i) {
            putchar(buffer[i]);
        }
    }
    fflush(stdout);
}

static bool read_line_with_mode(
    const char* prompt,
    char* out,
    size_t out_size,
    bool* hidden_mode,
    bool allow_toggle
) {
    if (out == nullptr || out_size < 2) {
        return false;
    }

    int pos = 0;
    bool hidden = (hidden_mode != nullptr) ? *hidden_mode : false;
    memset(out, 0, out_size);
    printf("%s", prompt);
    fflush(stdout);

    while (1) {
        int ch = getchar();
        if (ch == EOF || ch == -1) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (allow_toggle && ch == CTRL_T) {
            hidden = !hidden;
            printf("\r\n[password visibility: %s]\r\n", hidden ? "HIDDEN" : "VISIBLE");
            print_line_prompt(prompt, out, pos, hidden);
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            out[pos] = '\0';
            printf("\r\n");
            if (hidden_mode != nullptr) {
                *hidden_mode = hidden;
            }
            return true;
        }

        if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                out[pos] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (ch >= 32 && ch <= 126 && pos < (int)out_size - 1) {
            out[pos++] = static_cast<char>(ch);
            out[pos] = '\0';
            if (hidden) {
                putchar('*');
            } else {
                putchar(ch);
            }
            fflush(stdout);
        }
    }
}

static bool ask_yes_no_persist(const char* title) {
    char answer[16];
    while (1) {
        if (!read_line_with_mode(title, answer, sizeof(answer), nullptr, false)) {
            return false;
        }
        if (answer[0] == 'y' || answer[0] == 'Y') {
            return true;
        }
        if (answer[0] == 'n' || answer[0] == 'N') {
            return false;
        }
        printf("Please type y or n.\r\n");
    }
}

static void handle_wifi_setup(void) {
    if (g_wifi == nullptr) {
        ESP_LOGE(TAG, "WiFi module not initialized");
        return;
    }

    // На время мастера отключаем auto-reconnect, иначе scan может падать с ESP_ERR_WIFI_STATE.
    bool prev_auto_reconnect = g_wifi->get_auto_reconnect();
    g_wifi->set_auto_reconnect(false);
    g_wifi->abort_connecting();
    vTaskDelay(pdMS_TO_TICKS(200));

    std::vector<wifi_scan_record_t> nets;
    printf("\r\nScanning Wi-Fi networks...\r\n");
    if (!g_wifi->scan_networks(nets, 30)) {
        ESP_LOGE(TAG, "Wi-Fi scan failed");
        g_wifi->set_auto_reconnect(prev_auto_reconnect);
        return;
    }
    if (nets.empty()) {
        ESP_LOGW(TAG, "No Wi-Fi networks found");
        g_wifi->set_auto_reconnect(prev_auto_reconnect);
        return;
    }

    printf("Found %u networks:\r\n", (unsigned int)nets.size());
    for (size_t i = 0; i < nets.size(); ++i) {
        printf("%2u) %s | RSSI=%d | AUTH=%s | CH=%u\r\n",
               (unsigned int)i + 1,
               nets[i].ssid.c_str(),
               nets[i].rssi,
               auth_mode_to_str(nets[i].authmode),
               (unsigned int)nets[i].channel);
    }

    int selected = -1;
    char line[32];
    while (1) {
        if (!read_line_with_mode("Select network number: ", line, sizeof(line), nullptr, false)) {
            g_wifi->set_auto_reconnect(prev_auto_reconnect);
            return;
        }
        selected = atoi(line);
        if (selected >= 1 && selected <= (int)nets.size()) {
            break;
        }
        printf("Invalid number. Enter 1..%u\r\n", (unsigned int)nets.size());
    }

    bool hidden_mode = true;
    char pass[65] = {0};
    printf("Password input: Ctrl+T toggles visibility.\r\n");
    if (!read_line_with_mode("Enter password: ", pass, sizeof(pass), &hidden_mode, true)) {
        g_wifi->set_auto_reconnect(prev_auto_reconnect);
        return;
    }

    const wifi_scan_record_t& chosen = nets[(size_t)selected - 1];
    printf("Connecting to \"%s\" ...\r\n", chosen.ssid.c_str());
    wifi_connect_result_t result = g_wifi->connect_to(chosen.ssid.c_str(), pass, 15000);
    if (result == wifi_connect_result_t::SUCCESS) {
        ESP_LOGI(TAG, "Wi-Fi connected to %s", chosen.ssid.c_str());
        g_runtime_wifi_ssid = chosen.ssid;
        g_runtime_wifi_pass = pass;

        bool persist = ask_yes_no_persist("Save Wi-Fi config to NVS? (y/n): ");
        if (persist) {
            if (app_config_save_wifi(g_runtime_wifi_ssid.c_str(), g_runtime_wifi_pass.c_str())) {
                ESP_LOGI(TAG, "Wi-Fi config saved to NVS");
            } else {
                ESP_LOGE(TAG, "Failed to save Wi-Fi config to NVS");
            }
        } else {
            app_config_clear_wifi();
            ESP_LOGI(TAG, "Wi-Fi config set as runtime-only");
        }
    } else {
        ESP_LOGE(TAG, "Wi-Fi connect failed: %s", wifi_t::connect_result_to_string(result));
    }

    // Возвращаем авто-reconnect в исходное состояние после завершения мастера.
    g_wifi->set_auto_reconnect(prev_auto_reconnect);
}

static bool parse_server_set_args(const char* payload, char* out_ip, size_t out_ip_size, int* out_port) {
    if (payload == nullptr || out_ip == nullptr || out_port == nullptr) {
        return false;
    }
    char local[256];
    strncpy(local, payload, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';

    char* token = strtok(local, " ");
    if (token == nullptr || strcmp(token, "server_set") != 0) {
        return false;
    }
    char* ip = strtok(nullptr, " ");
    char* port = strtok(nullptr, " ");
    if (ip == nullptr || port == nullptr) {
        return false;
    }
    strncpy(out_ip, ip, out_ip_size - 1);
    out_ip[out_ip_size - 1] = '\0';
    *out_port = atoi(port);
    return true;
}

static void handle_server_set(const char* payload) {
    if (g_tcp == nullptr) {
        ESP_LOGE(TAG, "TCP module not initialized");
        return;
    }

    char ip[64] = {0};
    int port = 0;
    if (!parse_server_set_args(payload, ip, sizeof(ip), &port)) {
        if (!read_line_with_mode("Enter SERVER_IP: ", ip, sizeof(ip), nullptr, false)) {
            return;
        }
        char port_line[32] = {0};
        if (!read_line_with_mode("Enter SERVER_PORT: ", port_line, sizeof(port_line), nullptr, false)) {
            return;
        }
        port = atoi(port_line);
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        ESP_LOGE(TAG, "Invalid IP format: %s", ip);
        return;
    }
    if (port <= 0 || port > 65535) {
        ESP_LOGE(TAG, "Invalid port: %d", port);
        return;
    }

    if (!g_tcp->update_server_endpoint(ip, port, true)) {
        ESP_LOGE(TAG, "Failed to update server endpoint");
        return;
    }

    g_runtime_server_ip = ip;
    g_runtime_server_port = port;
    ESP_LOGI(TAG, "Server endpoint changed to %s:%d", ip, port);

    bool persist = ask_yes_no_persist("Save server config to NVS? (y/n): ");
    if (persist) {
        if (app_config_save_server(ip, port)) {
            ESP_LOGI(TAG, "Server config saved to NVS");
        } else {
            ESP_LOGE(TAG, "Failed to save server config to NVS");
        }
    } else {
        app_config_clear_server();
        ESP_LOGI(TAG, "Server config set as runtime-only");
    }
}

static void handle_server_saved_show(void) {
    app_config_t cfg;
    if (!app_config_load(cfg)) {
        ESP_LOGW(TAG, "NVS app config is not available");
        return;
    }

    if (!cfg.has_server) {
        ESP_LOGI(TAG, "Saved server endpoint: <not set>");
        return;
    }

    ESP_LOGI(TAG, "Saved server endpoint: %s:%d", cfg.server_ip.c_str(), cfg.server_port);
}

static void handle_server_saved_clear(void) {
    if (app_config_clear_server()) {
        ESP_LOGI(TAG, "Saved server endpoint removed from NVS");
    } else {
        ESP_LOGE(TAG, "Failed to remove saved server endpoint from NVS");
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
    printf("  Command: wifi_setup - interactive Wi-Fi setup\r\n");
    printf("  Command: server_set [ip] [port] - change TCP endpoint\r\n");
    printf("  Command: server_saved_show - show saved server endpoint in NVS\r\n");
    printf("  Command: server_saved_clear - remove saved server endpoint from NVS\r\n");
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
                
                // Обработка команд, которые доступны независимо от TCP connect
                if (strcmp(payload, "status") == 0) {
                    ESP_LOGI(TAG, "=== Status Information ===");
                    
                    ESP_LOGI(TAG, "WiFi:");
                    if (g_wifi) {
                        bool wifi_connected = g_wifi->is_connected();
                        ESP_LOGI(TAG, "  Connected: %s", wifi_connected ? "YES" : "NO");
                    } else {
                        ESP_LOGE(TAG, "  Status: NOT INITIALIZED");
                    }
                    
                    ESP_LOGI(TAG, "TCP:");
                    if (g_tcp) {
                        const char* tcp_state_str = g_tcp->get_state_string();
                        bool tcp_connected = g_tcp->is_connected();
                        ESP_LOGI(TAG, "  State: %s", tcp_state_str);
                        ESP_LOGI(TAG, "  Connected: %s", tcp_connected ? "YES" : "NO");
                        ESP_LOGI(TAG, "  Endpoint: %s:%d", g_tcp->get_server_ip(), g_tcp->get_server_port());
                    } else {
                        ESP_LOGE(TAG, "  Status: NOT INITIALIZED");
                    }
                    
                    printf("\r\n");
                    pos = 0;
                    continue;
                }

                if (strcmp(payload, "wifi_setup") == 0) {
                    handle_wifi_setup();
                    printf("\r\n");
                    pos = 0;
                    continue;
                }

                if (strncmp(payload, "server_set", 10) == 0) {
                    handle_server_set(payload);
                    printf("\r\n");
                    pos = 0;
                    continue;
                }

                if (strcmp(payload, "server_saved_show") == 0) {
                    handle_server_saved_show();
                    printf("\r\n");
                    pos = 0;
                    continue;
                }

                if (strcmp(payload, "server_saved_clear") == 0) {
                    handle_server_saved_clear();
                    printf("\r\n");
                    pos = 0;
                    continue;
                }

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

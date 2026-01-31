#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "nvs_flash.h"
#include "errno.h"

#include "netpack.h"

static const char* TAG = "vkorobka";

// Настройки Wi-Fi
#define WIFI_SSID "ModelSim"
#define WIFI_PASS "adminadmin"

// Настройки TCP
#define SERVER_IP   "192.168.43.236"
#define SERVER_PORT 1234

// Интервал переподключения (мс)
#define TCP_RECONNECT_DELAY_MS 3000

// Размер буфера для приема данных
#define TCP_RX_BUFFER_SIZE 4096

// State machine для TCP соединения
typedef enum {
    TCP_STATE_INIT,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_LOST,
    TCP_STATE_RETRY
} tcp_state_t;

// Callback для обработки принятых пакетов
typedef void (*packet_callback_t)(cmdcode_t cmd_code, const char* payload, u32 payload_len);

// Глобальные переменные
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static int g_tcp_sock = -1;
static tcp_state_t g_tcp_state = TCP_STATE_INIT;
static bool g_tcp_connected = false;
static packet_callback_t g_packet_callback = nullptr;
static std::vector<char> g_rx_buffer;  // Буфер для накопления входящих данных
static TaskHandle_t g_tcp_task_handle = nullptr;

// Обработчик событий Wi-Fi
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "Wi-Fi disconnected! Reason: %d", disconnected->reason);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Wi-Fi init done, waiting for connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// Проверка, что соединение активно
static bool tcp_connection_alive(int sock) {
    if (sock < 0) return false;
    
    // Используем MSG_PEEK для проверки без чтения данных
    char tmp;
    int ret = recv(sock, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    
    if (ret == 0) return false;  // Соединение закрыто
    if (ret < 0) {
        int err = errno;
        // EAGAIN/EWOULDBLOCK означает, что данных нет, но соединение активно
        // ENOTCONN означает, что сокет не подключен
        if (err == ENOTCONN || err == ECONNRESET || err == EPIPE) {
            return false;
        }
        return (err == EAGAIN || err == EWOULDBLOCK);
    }
    
    return true;
}

// Обработка буфера входящих данных - извлечение полных пакетов
static void process_rx_buffer(void) {
    u32 skip_count = 0;  // Счетчик пропущенных байт
    const u32 MAX_REASONABLE_PAYLOAD = 65535;  // Максимальная разумная длина payload
    const u32 MAX_SKIP_BEFORE_CLEAR = CMD_HEADER_LEN;  // После пропуска CMD_HEADER_LEN байт очищаем буфер
    
    while (g_rx_buffer.size() >= CMD_HEADER_LEN) {
        // Проверяем, есть ли полный заголовок
        const char* msg = g_rx_buffer.data();
        u32 msg_len = static_cast<u32>(g_rx_buffer.size());
        
        // Читаем заголовок для проверки длины (читаем байты напрямую для правильного порядка байт)
        // CMD Code: 2 байта (little-endian)
        // Payload length: 4 байта (little-endian)
        cmdcode_t cmd_code_raw = (cmdcode_t)(msg[0] | (msg[1] << 8));
        u32 payload_len_raw = (u32)(msg[2] | (msg[3] << 8) | (msg[4] << 16) | (msg[5] << 24));
        
        // Проверка на разумность payload_len перед дальнейшей обработкой
        if (payload_len_raw > MAX_REASONABLE_PAYLOAD) {
            // payload_len явно невалидный
            skip_count++;
            ESP_LOGW(TAG, "Invalid payload length: %u (too large), skipping byte (skip_count=%u)", 
                     payload_len_raw, skip_count);
            
            // Если пропустили слишком много байт, очищаем буфер полностью
            if (skip_count >= MAX_SKIP_BEFORE_CLEAR) {
                ESP_LOGW(TAG, "Too many invalid bytes, clearing RX buffer completely");
                g_rx_buffer.clear();
                skip_count = 0;
                break;
            }
            
            g_rx_buffer.erase(g_rx_buffer.begin());
            continue;
        }
        
        u32 expected_total = CMD_HEADER_LEN + payload_len_raw;
        
        ESP_LOGI(TAG, "Checking packet header:");
        ESP_LOGI(TAG, "  CMD Code: 0x%04x", cmd_code_raw);
        ESP_LOGI(TAG, "  Payload length (from header): %u bytes", payload_len_raw);
        ESP_LOGI(TAG, "  Expected total packet size: %u bytes", expected_total);
        ESP_LOGI(TAG, "  Current buffer size: %u bytes", msg_len);
        

        // Пытаемся распарсить пакет
        const char* payload_begin = nullptr;
        u32 payload_len = 0;
        cmdcode_t cmd_code = is_pack(msg, expected_total, &payload_begin, &payload_len);
        
        if (cmd_code != CMD_RESERVED) 
        {
            // Сбрасываем счетчик при успешном парсинге
            skip_count = 0;
            
            ESP_LOGI(TAG, "CMD format check PASSED");
            ESP_LOGI(TAG, "  Valid CMD Code: 0x%04x", cmd_code);
            ESP_LOGI(TAG, "  Payload length: %u bytes", payload_len);

            // Проверяем, есть ли полный пакет
            if (msg_len < expected_total) {
                // Недостаточно данных для полного пакета, ждем еще
                ESP_LOGI(TAG, "Incomplete packet, waiting for more data (need %u more bytes)", 
                        expected_total - msg_len);
                break;  // Выходим из цикла, чтобы дождаться больше данных
            }
            
            // Полный пакет получен и распарсен
            // Вызываем callback с данными пакета
            if (g_packet_callback) {
                if (payload_len > 0 && payload_begin != nullptr) {
                    g_packet_callback(cmd_code, payload_begin, payload_len);
                } else {
                    // Пакет без данных
                    g_packet_callback(cmd_code, nullptr, 0);
                }
            }
            
            // Удаляем весь обработанный пакет из буфера
            g_rx_buffer.erase(g_rx_buffer.begin(), g_rx_buffer.begin() + expected_total);
            continue;  // Продолжаем обработку следующего пакета
        }
        else
        {
            // Неверный формат пакета, пропускаем один байт и пытаемся снова
            skip_count++;
            ESP_LOGW(TAG, "Invalid packet format - CMD format check FAILED");
            ESP_LOGW(TAG, "  Header CMD Code: 0x%04x", cmd_code_raw);
            ESP_LOGW(TAG, "  Header Payload Length: %u", payload_len_raw);
            ESP_LOGW(TAG, "  Skipping byte and retrying... (skip_count=%u)", skip_count);
            
            // Если пропустили слишком много байт, очищаем буфер полностью
            if (skip_count >= MAX_SKIP_BEFORE_CLEAR) {
                ESP_LOGW(TAG, "Too many invalid bytes, clearing RX buffer completely");
                g_rx_buffer.clear();
                skip_count = 0;
                break;
            }
            
            g_rx_buffer.erase(g_rx_buffer.begin());
            continue;  // Продолжаем с пропущенным байтом
        }
    }
}

// Основная задача TCP клиента с автоматическим восстановлением соединения
static void tcp_client_task(void* pvParameters) {
    ESP_LOGI(TAG, "TCP client task started");
    
    tcp_state_t state = TCP_STATE_INIT;
    char rx_raw[TCP_RX_BUFFER_SIZE];
    
    while (1) {
        // Проверяем, что Wi-Fi подключен
        if ((xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
            ESP_LOGW(TAG, "Wi-Fi not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        switch (state) {
            case TCP_STATE_INIT: {
                ESP_LOGI(TAG, "TCP state: INIT");
                state = TCP_STATE_CONNECTING;
                break;
            }
            
            case TCP_STATE_CONNECTING: {
                ESP_LOGI(TAG, "TCP state: CONNECTING to %s:%d", SERVER_IP, SERVER_PORT);
                
                // Создаем сокет
                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                if (sock < 0) {
                    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                    state = TCP_STATE_RETRY;
                    break;
                }
                
                // Устанавливаем опции сокета
                int opt = 1;
                setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                
                // Устанавливаем таймауты
                struct timeval timeout;
                timeout.tv_sec = 5;
                timeout.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                
                // Настраиваем адрес сервера
                struct sockaddr_in dest_addr;
                dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(SERVER_PORT);
                
                // Подключаемся
                int err = connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
                if (err != 0) {
                    ESP_LOGE(TAG, "Socket connect failed: errno %d", errno);
                    close(sock);
                    state = TCP_STATE_RETRY;
                    break;
                }
                
                // Неблокирующий режим не нужен - используем MSG_DONTWAIT в recv
                
                g_tcp_sock = sock;
                g_tcp_connected = true;
                g_rx_buffer.clear();  // Очищаем буфер при новом соединении
                
                ESP_LOGI(TAG, "Connected successfully to %s:%d", SERVER_IP, SERVER_PORT);
                state = TCP_STATE_CONNECTED;
                break;
            }
            
            case TCP_STATE_CONNECTED: {
                // Проверяем, что соединение активно
                if (!tcp_connection_alive(g_tcp_sock)) {
                    ESP_LOGW(TAG, "Connection lost");
                    state = TCP_STATE_LOST;
                    break;
                }
                
                // Читаем входящие данные (неблокирующий режим)
                int len = recv(g_tcp_sock, rx_raw, sizeof(rx_raw), MSG_DONTWAIT);
                
                if (len > 0) {
                    ESP_LOGI(TAG, "Received %d bytes from socket", len);
                    
                    // Добавляем данные в буфер
                    g_rx_buffer.insert(g_rx_buffer.end(), rx_raw, rx_raw + len);
                    
                    ESP_LOGI(TAG, "RX buffer size: %u bytes", (unsigned int)g_rx_buffer.size());
                    
                    // Обрабатываем все полные пакеты из буфера
                    process_rx_buffer();
                } else if (len == 0) {
                    // Соединение закрыто сервером
                    ESP_LOGI(TAG, "Connection closed by server");
                    state = TCP_STATE_LOST;
                } else {
                    // Ошибка или нет данных (EAGAIN/EWOULDBLOCK)
                    int err = errno;
                    if (err != EAGAIN && err != EWOULDBLOCK) {
                        ESP_LOGE(TAG, "recv error: errno %d", err);
                        state = TCP_STATE_LOST;
                    }
                }
                
                // Небольшая задержка для снижения нагрузки на CPU
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
            
            case TCP_STATE_LOST: {
                ESP_LOGI(TAG, "TCP state: LOST");
                g_tcp_connected = false;
                
                if (g_tcp_sock >= 0) {
                    close(g_tcp_sock);
                    g_tcp_sock = -1;
                }
                
                g_rx_buffer.clear();
                state = TCP_STATE_RETRY;
                break;
            }
            
            case TCP_STATE_RETRY: {
                ESP_LOGI(TAG, "TCP state: RETRY (waiting %d ms)", TCP_RECONNECT_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(TCP_RECONNECT_DELAY_MS));
                state = TCP_STATE_INIT;
                break;
            }
        }
    }
    
    // Очистка при выходе
    if (g_tcp_sock >= 0) {
        close(g_tcp_sock);
        g_tcp_sock = -1;
    }
    vTaskDelete(NULL);
}

// Установка callback для обработки принятых пакетов
void tcp_set_packet_callback(packet_callback_t callback) {
    g_packet_callback = callback;
}

// Проверка, подключен ли TCP
bool tcp_is_connected(void) {
    return g_tcp_connected && (g_tcp_sock >= 0);
}

// Отправка данных через TCP (raw, без упаковки)
bool tcp_send(const char* data, int len) {
    if (!tcp_is_connected() || g_tcp_sock < 0) {
        ESP_LOGE(TAG, "Cannot send: not connected");
        return false;
    }
    
    int sent = send(g_tcp_sock, data, len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        return false;
    }
    
    return sent == len;
}

// Отправка пакета в формате CMD (с автоматической упаковкой)
bool tcp_send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len) {
    if (!tcp_is_connected() || g_tcp_sock < 0) {
        ESP_LOGE(TAG, "Cannot send packet: not connected");
        return false;
    }
    
    // Выделяем буфер для пакета
    std::vector<char> packet(CMD_HEADER_LEN + payload_len);
    
    // Упаковываем пакет
    u32 packet_size = pack_packet(cmd_code, payload, payload_len, packet.data());
    if (packet_size == 0) {
        ESP_LOGE(TAG, "Failed to pack packet");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending packet:");
    ESP_LOGI(TAG, "  CMD Code: 0x%04x", cmd_code);
    ESP_LOGI(TAG, "  Payload length: %u bytes", payload_len);
    ESP_LOGI(TAG, "  Total packet size: %u bytes", packet_size);
    
    // Отправляем все данные (может потребоваться несколько вызовов send)
    const char* ptr = packet.data();
    int remaining = static_cast<int>(packet_size);
    int total_sent = 0;
    
    while (remaining > 0) {
        int sent = send(g_tcp_sock, ptr, remaining, 0);
        if (sent < 0) {
            ESP_LOGE(TAG, "Send packet failed: errno %d", errno);
            return false;
        }
        if (sent == 0) {
            ESP_LOGE(TAG, "Connection closed during send");
            return false;
        }
        
        total_sent += sent;
        ptr += sent;
        remaining -= sent;
    }
    
    ESP_LOGI(TAG, "Packet sent successfully: %d bytes", total_sent);
    return true;
}

// Удобная перегрузка для отправки строки
bool tcp_send_packet_str(cmdcode_t cmd_code, const char* str) {
    return tcp_send_packet(cmd_code, str, static_cast<u32>(strlen(str)));
}

// ... existing code ...

// Обработчик принятых пакетов (пример)
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
    
    printf("Type message and press Enter to send:\r\n");
    printf("  Format: <data> - send raw data\r\n");
    printf("  Format: send <code> <data> - send CMD packet (code: 0x41 or 65)\r\n");
    
    while (1) {
        int ch = getchar();
        
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                payload[pos] = '\0';
                
                if (tcp_is_connected()) {
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
                                if (tcp_send_packet(cmd_code, data_ptr, data_len)) {
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
                            if (cmd_data_len > 0 && tcp_send(cmd_ptr, cmd_data_len)) {
                                ESP_LOGI(TAG, "Sent %d bytes (raw)", cmd_data_len);
                            } else {
                                ESP_LOGE(TAG, "Failed to send message");
                            }
                        }
                    } else {
                        // Отправляем как raw данные (как было в оригинале)
                        if (tcp_send(payload, pos)) {
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
    wifi_init_sta();
    ESP_LOGI(TAG, "Wi-Fi connected");
    
    // Устанавливаем обработчик принятых пакетов
    tcp_set_packet_callback(handle_packet);
    
    // Запускаем задачу TCP клиента
    xTaskCreate(tcp_client_task, "tcp_client", 8192, NULL, 5, &g_tcp_task_handle);
    ESP_LOGI(TAG, "TCP client task created");
    
    // Запускаем задачу для чтения консольного ввода
    xTaskCreate(console_input_task, "console_input", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Console input task created");
    
    // Основной цикл
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
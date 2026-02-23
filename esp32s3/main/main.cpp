#include <stdio.h>  // Для fgets
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "string.h"
#include "nvs_flash.h"

#include "ili9486_display.hpp"

// Режим отладки дисплея:
// если определить этот макрос (например, через CMake или просто раскомментировать строку),
// стек Wi‑Fi и TCP клиент НЕ будут запускаться, останется только работа дисплея.
#define LCD_DEBUG_NO_NET

// Режим ручного управления шиной (бит-бэнг через Parallel8Bus):
// если определить этот макрос, будет использоваться старый ручной драйвер вместо аппаратного I80.
// Это полезно для проверки физического подключения дисплея.
// #define USE_MANUAL_BUS

#ifdef USE_MANUAL_BUS
#include "parallel_bus.hpp"
#else
#include "esp_lcd_panel_io.h"
#include "i80_lcd_bus.hpp"
#endif

static const char* TAG = "vkorobka";

// Настройки Wi-Fi
#define WIFI_SSID "ModelSim"  // Замените на SSID хотспота телефона
#define WIFI_PASS "adminadmin"  // Замените на пароль

// Настройки TCP
#define SERVER_IP   "192.168.43.236"  // IP ПК (сервер), замените на реальный IP ПК из ipconfig
#define SERVER_PORT 1234              // Порт сервера

// Объекты дисплея
static Ili9486Display* s_lcd = nullptr;

// Цвета для теста: белый, синий, красный, чёрный, голубой (циан)
static const uint16_t s_test_colors[] = {
    0xFFFF, // WHITE
    0x001F, // BLUE
    0xF800, // RED
    0x01F0,
    0x07FF  // CYAN (голубой)
};
static constexpr size_t s_test_colors_count =
    sizeof(s_test_colors) / sizeof(s_test_colors[0]);

// Простая консоль для управления дисплеем через UART:
// Формат команд (вводить в монитор):
//   R x y w h color_hex   - залить прямоугольник (десятичные x,y,w,h, цвет в hex, например F800)
//   C color_hex           - залить весь экран цветом
extern "C" void display_test_task(void* pvParameters) {
    // Ждём, пока дисплей будет инициализирован
    while (s_lcd == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "LCD CONSOLE: ready. Commands:");
    ESP_LOGI(TAG, "  R x y w h color_hex   - fill rect");
    ESP_LOGI(TAG, "  C color_hex           - fill screen");
    printf("\r\nLCD> ");
    fflush(stdout);

    char line[64];
    size_t pos = 0;

    while (true) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            // Завершаем строку
            line[pos] = '\0';

            if (pos > 0) {
                char cmd = 0;
                unsigned x = 0, y = 0, w = 0, h = 0;
                unsigned color = 0;

                int n = sscanf(line, " %c", &cmd);
                if (n == 1) {
                    if (cmd == 'R' || cmd == 'r') {
                        if (sscanf(line, " %c %u %u %u %u %x",
                                   &cmd, &x, &y, &w, &h, &color) == 6) {
                            ESP_LOGI(TAG, "LCD CMD: RECT x=%u y=%u w=%u h=%u color=0x%04X",
                                     x, y, w, h, (unsigned)(color & 0xFFFF));
                            if (s_lcd) {
                                s_lcd->fillRect((uint16_t)x, (uint16_t)y,
                                                (uint16_t)w, (uint16_t)h,
                                                (uint16_t)(color & 0xFFFF));
                            }
                        } else {
                            ESP_LOGW(TAG, "LCD CMD: bad RECT, use: R x y w h color_hex");
                        }
                    } else if (cmd == 'C' || cmd == 'c') {
                        if (sscanf(line, " %c %x", &cmd, &color) == 2) {
                            ESP_LOGI(TAG, "LCD CMD: CLEAR color=0x%04X",
                                     (unsigned)(color & 0xFFFF));
                            if (s_lcd) {
                                s_lcd->fillScreen((uint16_t)(color & 0xFFFF));
                            }
                        } else {
                            ESP_LOGW(TAG, "LCD CMD: bad CLEAR, use: C color_hex");
                        }
                    } else {
                        ESP_LOGW(TAG, "LCD CMD: unknown command '%c'", cmd);
                    }
                }
            }

            // Новая строка и приглашение
            printf("\r\nLCD> ");
            fflush(stdout);
            pos = 0;
        } else if ((ch == '\b' || ch == 127)) {
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (ch >= 32 && ch <= 126 && pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Обработчик событий Wi-Fi
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "Disconnected! Reason: %d (%s)", disconnected->reason,
                 esp_err_to_name(disconnected->reason));  // Логирование причины

        esp_wifi_connect();  // retry
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
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;  // Принудительно WPA2

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Wi-Fi init done, waiting for connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// Задача приема сообщений
static void tcp_rx_task(void* pvParameters) {
    int sock = *(int*)pvParameters;
    char rx_buffer[256];

    while (1) {
        int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Recv failed: errno %d", errno);
            break;
        } else if (len == 0) {
            ESP_LOGI(TAG, "Connection closed");
            break;
        } else {
            rx_buffer[len] = 0;
            ESP_LOGI(TAG, "PC: %s", rx_buffer);
        }
    }
    vTaskDelete(NULL);
}

static void tcp_client_task(void* pvParameters) {
    ESP_LOGI(TAG, "Starting TCP client...");

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    ESP_LOGI(TAG, "Connecting to %s:%d...", SERVER_IP, SERVER_PORT);
    int err = connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket connect failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Connected successfully! Starting RX task...");

    // Запускаем приём в отдельной задаче
    xTaskCreate(tcp_rx_task, "tcp_rx", 4096, &sock, 5, NULL);

    // Отправка сообщений с нормальным редактированием
    char payload[256];
    int pos = 0;

    printf("Type message and press Enter to send:\r\n");

    while (1) {
        int ch = getchar();

        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                payload[pos] = '\0';
                int len = send(sock, payload, pos, 0);
                if (len < 0) {
                    ESP_LOGE(TAG, "Send failed: errno %d", errno);
                    break;
                }
                ESP_LOGI(TAG, "Sent %d bytes: %s", len, payload);
                printf("\r\n");           // новая строка после отправки
                pos = 0;
            }
        }
        else if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                printf("\b \b");          // стираем символ визуально
                fflush(stdout);
            }
        }
        else if (ch >= 32 && ch <= 126 && pos < sizeof(payload) - 1) {
            payload[pos++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "vkorobka: App starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing... ret = 0x%x", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized successfully");

    // Инициализация дисплея (ручной или аппаратный режим)
#ifdef USE_MANUAL_BUS
    ESP_LOGI(TAG, "LCD: Using MANUAL bus (Parallel8Bus bit-bang)");
    static Parallel8Bus lcdBus;
    ESP_ERROR_CHECK(lcdBus.init());
    static Ili9486Display lcd(lcdBus);
    s_lcd = &lcd;
    ESP_LOGI(TAG, "LCD: init controller...");
    lcd.init();
#else
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
#endif

    // Инициализация сетевого стека (может быть отключена в режиме отладки дисплея)
#ifndef LCD_DEBUG_NO_NET
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_sta();
    ESP_LOGI(TAG, "vkorobka: Network ready, starting TCP client task...");

    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
#else
    ESP_LOGW(TAG, "LCD DEBUG MODE: Wi-Fi/TCP stack NOT started (LCD_DEBUG_NO_NET)");
#endif

    // Тестовая задача заливки дисплея
    xTaskCreate(display_test_task, "display_test", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "vkorobka: app_main finished");
}
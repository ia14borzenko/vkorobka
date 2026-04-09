#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "ili9486_display.hpp"
#include "esp_lcd_panel_io.h"
#include "i80_lcd_bus.hpp"
#include "wifi.hpp"
#include "tcp.hpp"
#include "message_bridge.hpp"
#include "message_protocol.h"
#include "netpack.h"
#include "my_types.h"
#include "texting.hpp"
#include "stream_handler.hpp"
#include "command_handler.hpp"
#include "console_input.hpp"
#include "message_dispatcher.hpp"
#include "mic_stream.hpp"
#include "dyn_playback.hpp"
#include "app_config.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <string>

static const char* TAG = "vkorobka";

// Режим отладки дисплея:
// если определить этот макрос (например, через CMake или просто раскомментировать строку),
// стек Wi‑Fi и TCP клиент НЕ будут запускаться, останется только работа дисплея.
// #define LCD_DEBUG_NO_NET

// Настройки Wi-Fi
#define WIFI_SSID "ModelSim"
#define WIFI_PASS "adminadmin"

// Настройки TCP
#define SERVER_IP   "192.168.43.22"
#define SERVER_PORT 1234

// Глобальные объекты
Ili9486Display* s_lcd = nullptr;  // Глобальный указатель на дисплей
wifi_t* g_wifi = nullptr;
tcp_t* g_tcp = nullptr;
message_bridge_t* g_message_bridge = nullptr;
std::string g_runtime_wifi_ssid = WIFI_SSID;
std::string g_runtime_wifi_pass = WIFI_PASS;
std::string g_runtime_server_ip = SERVER_IP;
int g_runtime_server_port = SERVER_PORT;
static bool g_loaded_wifi_from_nvs = false;
static bool g_loaded_server_from_nvs = false;
static bool g_server_fallback_done = false;
static TickType_t g_server_connect_started_tick = 0;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t SERVER_FALLBACK_TIMEOUT_MS = 20000;  // 20 sec

// Инициализация приложения
static void app_init() {
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

    app_config_t loaded_cfg;
    if (app_config_load(loaded_cfg))
    {
        if (loaded_cfg.has_wifi)
        {
            g_runtime_wifi_ssid = loaded_cfg.wifi_ssid;
            g_runtime_wifi_pass = loaded_cfg.wifi_pass;
            g_loaded_wifi_from_nvs = true;
            ESP_LOGI(TAG, "Loaded Wi-Fi config from NVS: SSID=%s", g_runtime_wifi_ssid.c_str());
        }
        if (loaded_cfg.has_server)
        {
            g_runtime_server_ip = loaded_cfg.server_ip;
            g_runtime_server_port = loaded_cfg.server_port;
            g_loaded_server_from_nvs = true;
            ESP_LOGI(TAG, "Loaded server config from NVS: %s:%d", g_runtime_server_ip.c_str(), g_runtime_server_port);
        }
    }

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

    // Инициализация модулей
    texting_init(s_lcd);
    stream_handler_init(s_lcd);

    // Инициализация сетевого стека (может быть отключена в режиме отладки дисплея)
#ifndef LCD_DEBUG_NO_NET
    // Инициализация сетевого стека
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    
    // Инициализация Wi-Fi
    g_wifi = new wifi_t();
    bool wifi_initialized = g_wifi->init(WIFI_SSID, WIFI_PASS);
    if (!wifi_initialized) {
        ESP_LOGE(TAG, "WiFi initialization failed, network features will be unavailable until reboot");
    }

    wifi_connect_result_t wifi_result = wifi_connect_result_t::SUCCESS;
    if (wifi_initialized && g_loaded_wifi_from_nvs)
    {
        ESP_LOGI(TAG, "Trying Wi-Fi from NVS first...");
        wifi_result = g_wifi->connect_to(g_runtime_wifi_ssid.c_str(), g_runtime_wifi_pass.c_str(), WIFI_CONNECT_TIMEOUT_MS);
        if (wifi_result != wifi_connect_result_t::SUCCESS)
        {
            ESP_LOGW(TAG, "NVS Wi-Fi connect failed (%s). Falling back to firmware constants...",
                     wifi_t::connect_result_to_string(wifi_result));
            wifi_result = g_wifi->connect_to(WIFI_SSID, WIFI_PASS, WIFI_CONNECT_TIMEOUT_MS);
            if (wifi_result == wifi_connect_result_t::SUCCESS)
            {
                g_runtime_wifi_ssid = WIFI_SSID;
                g_runtime_wifi_pass = WIFI_PASS;
                ESP_LOGI(TAG, "Connected using firmware Wi-Fi constants");
            }
        }
    }
    else if (wifi_initialized)
    {
        ESP_LOGI(TAG, "Trying firmware Wi-Fi constants...");
        wifi_result = g_wifi->connect_to(WIFI_SSID, WIFI_PASS, WIFI_CONNECT_TIMEOUT_MS);
        if (wifi_result == wifi_connect_result_t::SUCCESS)
        {
            g_runtime_wifi_ssid = WIFI_SSID;
            g_runtime_wifi_pass = WIFI_PASS;
        }
    }

    if (wifi_initialized && wifi_result != wifi_connect_result_t::SUCCESS)
    {
        ESP_LOGW(TAG, "Wi-Fi connect failed: %s", wifi_t::connect_result_to_string(wifi_result));
        ESP_LOGW(TAG, "Console remains available: use 'wifi_setup' to connect manually");
    }
    if (wifi_initialized && wifi_result == wifi_connect_result_t::SUCCESS)
    {
        ESP_LOGI(TAG, "Wi-Fi connected (SSID=%s)", g_runtime_wifi_ssid.c_str());
    }
    
    // Инициализация message bridge
    g_message_bridge = new message_bridge_t();
    g_message_bridge->init(nullptr);  // TCP будет установлен позже
    g_message_bridge->register_handler(MSG_TYPE_COMMAND, message_dispatcher_handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_DATA, message_dispatcher_handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_STREAM, message_dispatcher_handle_new_message);
    
    // Инициализация TCP
    g_tcp = new tcp_t(g_runtime_server_ip.c_str(), g_runtime_server_port, g_wifi);
    g_tcp->set_packet_callback(message_dispatcher_handle_packet);
    
    // Устанавливаем TCP в message_bridge
    g_message_bridge->init(g_tcp);
    
    if (!g_tcp->start()) {
        ESP_LOGE(TAG, "TCP client start failed");
    } else {
        g_server_connect_started_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "TCP client started");
    }

    mic_stream_start();
    dyn_playback_init();

    // Запускаем задачу для чтения консольного ввода
    console_input_init();
#else
    ESP_LOGW(TAG, "LCD DEBUG MODE: Wi-Fi/TCP stack NOT started (LCD_DEBUG_NO_NET)");
#endif
}

// Основной цикл выполнения
static void app_exec() {
#ifndef LCD_DEBUG_NO_NET
    // Основной цикл
    while (1) {
        if (g_loaded_server_from_nvs && !g_server_fallback_done && g_tcp != nullptr)
        {
            if (!g_tcp->is_connected())
            {
                TickType_t now_tick = xTaskGetTickCount();
                TickType_t elapsed_ticks = now_tick - g_server_connect_started_tick;
                if (elapsed_ticks >= pdMS_TO_TICKS(SERVER_FALLBACK_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "NVS server endpoint is not reachable. Falling back to firmware constants...");
                    if (g_tcp->update_server_endpoint(SERVER_IP, SERVER_PORT, true))
                    {
                        g_runtime_server_ip = SERVER_IP;
                        g_runtime_server_port = SERVER_PORT;
                        ESP_LOGI(TAG, "Fallback server endpoint applied: %s:%d", SERVER_IP, SERVER_PORT);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to apply fallback server endpoint");
                    }
                    g_server_fallback_done = true;
                }
            }
            else
            {
                g_server_fallback_done = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    // В режиме отладки дисплея просто остаёмся в бесконечном цикле,
    // чтобы задача app_main не завершилась сразу.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

extern "C" void app_main(void) {
    app_init();
    app_exec();
}

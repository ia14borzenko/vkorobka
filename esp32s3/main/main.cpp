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
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static const char* TAG = "vkorobka";

// Режим отладки дисплея:
// если определить этот макрос (например, через CMake или просто раскомментировать строку),
// стек Wi‑Fi и TCP клиент НЕ будут запускаться, останется только работа дисплея.
// #define LCD_DEBUG_NO_NET

// Настройки Wi-Fi
#define WIFI_SSID "ModelSim"
#define WIFI_PASS "adminadmin"

// Настройки TCP
#define SERVER_IP   "192.168.43.236"
#define SERVER_PORT 1234

// Глобальные объекты
Ili9486Display* s_lcd = nullptr;  // Глобальный указатель на дисплей
wifi_t* g_wifi = nullptr;
tcp_t* g_tcp = nullptr;
message_bridge_t* g_message_bridge = nullptr;

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
    if (!g_wifi->init(WIFI_SSID, WIFI_PASS)) {
        ESP_LOGE(TAG, "WiFi initialization failed");
        return;
    }
    g_wifi->wait_for_connection();
    ESP_LOGI(TAG, "Wi-Fi connected");
    
    // Инициализация message bridge
    g_message_bridge = new message_bridge_t();
    g_message_bridge->init(nullptr);  // TCP будет установлен позже
    g_message_bridge->register_handler(MSG_TYPE_COMMAND, message_dispatcher_handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_DATA, message_dispatcher_handle_new_message);
    g_message_bridge->register_handler(MSG_TYPE_STREAM, message_dispatcher_handle_new_message);
    
    // Инициализация TCP
    g_tcp = new tcp_t(SERVER_IP, SERVER_PORT, g_wifi);
    g_tcp->set_packet_callback(message_dispatcher_handle_packet);
    
    // Устанавливаем TCP в message_bridge
    g_message_bridge->init(g_tcp);
    
    if (!g_tcp->start()) {
        ESP_LOGE(TAG, "TCP client start failed");
        return;
    }
    ESP_LOGI(TAG, "TCP client started");

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

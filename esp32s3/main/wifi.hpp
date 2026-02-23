#ifndef WIFI_HPP
#define WIFI_HPP

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_wifi.h"

// Callback для обработки событий WiFi
typedef void (*wifi_event_callback_t)(void);

class wifi_t
{
public:
    wifi_t();
    ~wifi_t();

    // Инициализация и подключение к WiFi
    bool init(const char* ssid, const char* password);
    
    // Ожидание подключения (блокирующий вызов)
    void wait_for_connection(void);
    
    // Проверка, подключен ли WiFi
    bool is_connected(void) const;
    
    // Получить EventGroup для проверки состояния
    EventGroupHandle_t get_event_group(void) const;

private:
    EventGroupHandle_t event_group;
    bool initialized;
    
    static void event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data);
    
    static constexpr int WIFI_CONNECTED_BIT = BIT0;
};

#endif // WIFI_HPP

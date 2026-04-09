#ifndef WIFI_HPP
#define WIFI_HPP

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <string>
#include <vector>

// Callback для обработки событий WiFi
typedef void (*wifi_event_callback_t)(void);

struct wifi_scan_record_t {
    std::string ssid;
    int rssi;
    wifi_auth_mode_t authmode;
    uint8_t channel;
};

enum class wifi_connect_result_t {
    SUCCESS = 0,
    NOT_INITIALIZED,
    INVALID_ARG,
    TIMEOUT,
    AUTH_FAIL,
    AP_NOT_FOUND,
    WIFI_ERROR
};

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

    // Сканирование доступных сетей
    bool scan_networks(std::vector<wifi_scan_record_t>& out_networks, uint16_t max_networks = 20);

    // Подключение к указанной сети с таймаутом
    wifi_connect_result_t connect_to(const char* ssid, const char* password, uint32_t timeout_ms);

    // Преобразование результата подключения в текст
    static const char* connect_result_to_string(wifi_connect_result_t result);

    // Управление авто-reconnect
    void set_auto_reconnect(bool enabled);
    bool get_auto_reconnect(void) const;

    // Принудительно прервать текущую попытку подключения
    bool abort_connecting(void);
    
    // Получить EventGroup для проверки состояния
    EventGroupHandle_t get_event_group(void) const;

private:
    EventGroupHandle_t event_group;
    bool initialized;
    bool manual_connect_in_progress;
    bool auto_reconnect_enabled;
    uint8_t last_disconnect_reason;
    
    static void event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data);
    
    static constexpr int WIFI_CONNECTED_BIT = BIT0;
    static constexpr int WIFI_CONNECT_FAIL_BIT = BIT1;
};

#endif // WIFI_HPP

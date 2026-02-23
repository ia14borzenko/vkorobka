#include "wifi.hpp"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>

static const char* TAG = "wifi";

wifi_t::wifi_t()
    : event_group(nullptr)
    , initialized(false)
{
}

wifi_t::~wifi_t()
{
    if (event_group != nullptr)
    {
        vEventGroupDelete(event_group);
        event_group = nullptr;
    }
}

bool wifi_t::init(const char* ssid, const char* password)
{
    if (initialized)
    {
        ESP_LOGW(TAG, "WiFi already initialized");
        return true;
    }

    event_group = xEventGroupCreate();
    if (event_group == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        vEventGroupDelete(event_group);
        event_group = nullptr;
        return false;
    }

    // Регистрируем обработчики событий
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                               &wifi_t::event_handler, this, &instance_any_id);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        vEventGroupDelete(event_group);
        event_group = nullptr;
        return false;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                               &wifi_t::event_handler, this, &instance_got_ip);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        vEventGroupDelete(event_group);
        event_group = nullptr;
        return false;
    }

    // Настраиваем WiFi
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        vEventGroupDelete(event_group);
        event_group = nullptr;
        return false;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        vEventGroupDelete(event_group);
        event_group = nullptr;
        return false;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        esp_wifi_deinit();
        vEventGroupDelete(event_group);
        event_group = nullptr;
        return false;
    }

    initialized = true;
    ESP_LOGI(TAG, "WiFi initialized, connecting to SSID: %s", ssid);
    
    // esp_wifi_connect() будет вызван автоматически в event_handler при WIFI_EVENT_STA_START
    // Не вызываем здесь, чтобы избежать двойного вызова

    return true;
}

void wifi_t::wait_for_connection(void)
{
    if (event_group == nullptr)
    {
        ESP_LOGE(TAG, "WiFi not initialized");
        return;
    }

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(event_group, wifi_t::WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");
}

bool wifi_t::is_connected(void) const
{
    if (event_group == nullptr)
    {
        return false;
    }
    
    return (xEventGroupGetBits(event_group) & wifi_t::WIFI_CONNECTED_BIT) != 0;
}

EventGroupHandle_t wifi_t::get_event_group(void) const
{
    return event_group;
}

void wifi_t::event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    wifi_t* wifi = static_cast<wifi_t*>(arg);
    
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "WiFi station started");
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            wifi_event_sta_disconnected_t* disconnected = 
                (wifi_event_sta_disconnected_t*)event_data;
            ESP_LOGE(TAG, "WiFi disconnected! Reason: %d", disconnected->reason);
            
            if (wifi->event_group != nullptr)
            {
                xEventGroupClearBits(wifi->event_group, wifi_t::WIFI_CONNECTED_BIT);
            }
            
            esp_wifi_connect();
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            
            if (wifi->event_group != nullptr)
            {
                xEventGroupSetBits(wifi->event_group, wifi_t::WIFI_CONNECTED_BIT);
            }
        }
    }
}

#include "wifi.hpp"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>

static const char* TAG = "wifi";

static bool is_auth_failure_reason(uint8_t reason)
{
    if (reason == WIFI_REASON_AUTH_FAIL) return true;
#ifdef WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) return true;
#endif
#ifdef WIFI_REASON_HANDSHAKE_TIMEOUT
    if (reason == WIFI_REASON_HANDSHAKE_TIMEOUT) return true;
#endif
    return false;
}

static bool is_ap_not_found_reason(uint8_t reason)
{
    if (reason == WIFI_REASON_NO_AP_FOUND) return true;
#ifdef WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    if (reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD) return true;
#endif
#ifdef WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
    if (reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) return true;
#endif
    return false;
}

wifi_t::wifi_t()
    : event_group(nullptr)
    , initialized(false)
    , manual_connect_in_progress(false)
    , auto_reconnect_enabled(true)
    , last_disconnect_reason(0)
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

bool wifi_t::scan_networks(std::vector<wifi_scan_record_t>& out_networks, uint16_t max_networks)
{
    out_networks.clear();
    if (!initialized)
    {
        ESP_LOGE(TAG, "Cannot scan: WiFi not initialized");
        return false;
    }

    esp_err_t ret = esp_wifi_scan_start(nullptr, true);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        return false;
    }

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (ap_count == 0)
    {
        return true;
    }

    uint16_t records_count = ap_count;
    if (records_count > max_networks)
    {
        records_count = max_networks;
    }

    std::vector<wifi_ap_record_t> records(records_count);
    ret = esp_wifi_scan_get_ap_records(&records_count, records.data());
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
        return false;
    }

    out_networks.reserve(records_count);
    for (uint16_t i = 0; i < records_count; ++i)
    {
        wifi_scan_record_t entry;
        entry.ssid = reinterpret_cast<const char*>(records[i].ssid);
        entry.rssi = records[i].rssi;
        entry.authmode = records[i].authmode;
        entry.channel = records[i].primary;
        out_networks.push_back(entry);
    }

    return true;
}

wifi_connect_result_t wifi_t::connect_to(const char* ssid, const char* password, uint32_t timeout_ms)
{
    if (!initialized)
    {
        return wifi_connect_result_t::NOT_INITIALIZED;
    }
    if (ssid == nullptr || ssid[0] == '\0')
    {
        return wifi_connect_result_t::INVALID_ARG;
    }
    if (password == nullptr)
    {
        return wifi_connect_result_t::INVALID_ARG;
    }

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    last_disconnect_reason = 0;
    manual_connect_in_progress = true;
    xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT);

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT)
    {
        ESP_LOGW(TAG, "esp_wifi_disconnect returned: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK)
    {
        manual_connect_in_progress = false;
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return wifi_connect_result_t::WIFI_ERROR;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK)
    {
        manual_connect_in_progress = false;
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        return wifi_connect_result_t::WIFI_ERROR;
    }

    EventBits_t bits = xEventGroupWaitBits(
        event_group,
        WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );

    manual_connect_in_progress = false;

    if ((bits & WIFI_CONNECTED_BIT) != 0)
    {
        return wifi_connect_result_t::SUCCESS;
    }
    if ((bits & WIFI_CONNECT_FAIL_BIT) != 0)
    {
        if (is_auth_failure_reason(last_disconnect_reason))
        {
            return wifi_connect_result_t::AUTH_FAIL;
        }
        if (is_ap_not_found_reason(last_disconnect_reason))
        {
            return wifi_connect_result_t::AP_NOT_FOUND;
        }
        return wifi_connect_result_t::WIFI_ERROR;
    }

    return wifi_connect_result_t::TIMEOUT;
}

const char* wifi_t::connect_result_to_string(wifi_connect_result_t result)
{
    switch (result)
    {
    case wifi_connect_result_t::SUCCESS:
        return "SUCCESS";
    case wifi_connect_result_t::NOT_INITIALIZED:
        return "NOT_INITIALIZED";
    case wifi_connect_result_t::INVALID_ARG:
        return "INVALID_ARG";
    case wifi_connect_result_t::TIMEOUT:
        return "TIMEOUT";
    case wifi_connect_result_t::AUTH_FAIL:
        return "AUTH_FAIL";
    case wifi_connect_result_t::AP_NOT_FOUND:
        return "AP_NOT_FOUND";
    case wifi_connect_result_t::WIFI_ERROR:
        return "WIFI_ERROR";
    default:
        return "UNKNOWN";
    }
}

void wifi_t::set_auto_reconnect(bool enabled)
{
    auto_reconnect_enabled = enabled;
}

bool wifi_t::get_auto_reconnect(void) const
{
    return auto_reconnect_enabled;
}

bool wifi_t::abort_connecting(void)
{
    if (!initialized)
    {
        return false;
    }
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK || ret == ESP_ERR_WIFI_NOT_CONNECT)
    {
        return true;
    }
    ESP_LOGW(TAG, "abort_connecting failed: %s", esp_err_to_name(ret));
    return false;
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
            wifi->last_disconnect_reason = disconnected->reason;
            
            if (wifi->event_group != nullptr)
            {
                xEventGroupClearBits(wifi->event_group, wifi_t::WIFI_CONNECTED_BIT);
                if (wifi->manual_connect_in_progress)
                {
                    xEventGroupSetBits(wifi->event_group, wifi_t::WIFI_CONNECT_FAIL_BIT);
                }
            }

            if (!wifi->manual_connect_in_progress && wifi->auto_reconnect_enabled)
            {
                esp_wifi_connect();
            }
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
                xEventGroupClearBits(wifi->event_group, wifi_t::WIFI_CONNECT_FAIL_BIT);
            }
        }
    }
}

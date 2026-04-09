#include "app_config.hpp"
#include "nvs.h"
#include "esp_log.h"
#include <vector>

static const char* TAG = "app_config";
static const char* NS = "app_cfg";
static const char* KEY_WIFI_SSID = "wifi_ssid";
static const char* KEY_WIFI_PASS = "wifi_pass";
static const char* KEY_SERVER_IP = "srv_ip";
static const char* KEY_SERVER_PORT = "srv_port";

static bool read_string(nvs_handle_t handle, const char* key, std::string& out)
{
    size_t len = 0;
    esp_err_t ret = nvs_get_str(handle, key, nullptr, &len);
    if (ret != ESP_OK)
    {
        return false;
    }

    std::vector<char> buf(len, 0);
    ret = nvs_get_str(handle, key, buf.data(), &len);
    if (ret != ESP_OK)
    {
        return false;
    }

    out.assign(buf.data());
    return true;
}

bool app_config_load(app_config_t& out_cfg)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NS, NVS_READONLY, &handle);
    if (ret != ESP_OK)
    {
        return false;
    }

    out_cfg.has_wifi = read_string(handle, KEY_WIFI_SSID, out_cfg.wifi_ssid) &&
                       read_string(handle, KEY_WIFI_PASS, out_cfg.wifi_pass);
    out_cfg.has_server = read_string(handle, KEY_SERVER_IP, out_cfg.server_ip);

    int32_t port = 0;
    ret = nvs_get_i32(handle, KEY_SERVER_PORT, &port);
    if (ret != ESP_OK)
    {
        out_cfg.has_server = false;
    }
    else
    {
        out_cfg.server_port = static_cast<int>(port);
        if (out_cfg.server_port <= 0 || out_cfg.server_port > 65535)
        {
            out_cfg.has_server = false;
        }
    }

    nvs_close(handle);
    return true;
}

bool app_config_save_wifi(const char* ssid, const char* password)
{
    if (ssid == nullptr || password == nullptr || ssid[0] == '\0')
    {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (ret == ESP_OK)
    {
        ret = nvs_set_str(handle, KEY_WIFI_PASS, password);
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret == ESP_OK;
}

bool app_config_clear_wifi(void)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        return false;
    }

    esp_err_t ret1 = nvs_erase_key(handle, KEY_WIFI_SSID);
    esp_err_t ret2 = nvs_erase_key(handle, KEY_WIFI_PASS);
    if ((ret1 == ESP_OK || ret1 == ESP_ERR_NVS_NOT_FOUND) &&
        (ret2 == ESP_OK || ret2 == ESP_ERR_NVS_NOT_FOUND))
    {
        ret = nvs_commit(handle);
    }
    else
    {
        ret = ESP_FAIL;
    }

    nvs_close(handle);
    return ret == ESP_OK;
}

bool app_config_save_server(const char* server_ip, int server_port)
{
    if (server_ip == nullptr || server_ip[0] == '\0' || server_port <= 0 || server_port > 65535)
    {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = nvs_set_str(handle, KEY_SERVER_IP, server_ip);
    if (ret == ESP_OK)
    {
        ret = nvs_set_i32(handle, KEY_SERVER_PORT, static_cast<int32_t>(server_port));
    }
    if (ret == ESP_OK)
    {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret == ESP_OK;
}

bool app_config_clear_server(void)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(NS, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
    {
        return false;
    }

    esp_err_t ret1 = nvs_erase_key(handle, KEY_SERVER_IP);
    esp_err_t ret2 = nvs_erase_key(handle, KEY_SERVER_PORT);
    if ((ret1 == ESP_OK || ret1 == ESP_ERR_NVS_NOT_FOUND) &&
        (ret2 == ESP_OK || ret2 == ESP_ERR_NVS_NOT_FOUND))
    {
        ret = nvs_commit(handle);
    }
    else
    {
        ret = ESP_FAIL;
    }

    nvs_close(handle);
    return ret == ESP_OK;
}

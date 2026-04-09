#pragma once

#include <string>

struct app_config_t {
    std::string wifi_ssid;
    std::string wifi_pass;
    std::string server_ip;
    int server_port = 0;
    bool has_wifi = false;
    bool has_server = false;
};

bool app_config_load(app_config_t& out_cfg);
bool app_config_save_wifi(const char* ssid, const char* password);
bool app_config_clear_wifi(void);
bool app_config_save_server(const char* server_ip, int server_port);
bool app_config_clear_server(void);

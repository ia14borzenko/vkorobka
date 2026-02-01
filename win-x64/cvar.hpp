#ifndef CVAR_HPP
#define CVAR_HPP

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>

#include <winsock2.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>
#include "tcp.hpp"
#include "wifi.hpp"
#include "netpack.h"

// Внешние переменные для синхронизации с командой test_esp32 (определены в main.cpp)
extern std::mutex g_response_mutex;
extern std::condition_variable g_response_cv;
extern std::atomic<bool> g_test_response_received;
extern std::atomic<bool> g_status_response_received;
extern std::string g_esp32_status_response;

#define ANSI_FG_PREFIX "3"
#define ANSI_FG_BRIGHT_PREFIX "9"
#define ANSI_BG_PREFIX "4"
#define ANSI_BG_BRIGHT_PREFIX "10"

#define ANSI_BLACK   "0"
#define ANSI_RED     "1"
#define ANSI_GREEN   "2"
#define ANSI_YELLOW  "3"
#define ANSI_BLUE    "4"
#define ANSI_MAGENTA "5"
#define ANSI_CYAN    "6"
#define ANSI_WHITE   "7"


#define ANSI_ESC "\x1B["
#define ANSI_END "m"

#define ANSI_ENDL ANSI_ESC "0" ANSI_END
#define ANSI_COLOR(fg, bg) ANSI_ESC ANSI_FG_PREFIX fg ";" ANSI_BG_PREFIX bg ANSI_END

#define ANSI_INFO ANSI_COLOR(ANSI_BLUE, ANSI_BLACK)
#define ANSI_WARN ANSI_COLOR(ANSI_YELLOW, ANSI_BLACK)
#define ANSI_ERR ANSI_COLOR(ANSI_RED, ANSI_WHITE)
#define ANSI_SUCC ANSI_COLOR(ANSI_GREEN, ANSI_BLACK)

/* ============================================================
   Context of console input
   ============================================================ */

struct cctx_t
{
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
    tcp_t* tcp;  // Указатель на TCP объект для отправки данных
    wifi_t* wifi;  // Указатель на WiFi объект для получения состояния

    const std::string& get(size_t index) const
    {
        static const std::string empty;
        if (index >= positional.size())
            return empty;
        return positional[index];
    }

    bool has_flag(const std::string& name) const
    {
        return flags.find(name) != flags.end();
    }

    const std::string& flag(const std::string& name) const
    {
        static const std::string empty;
        auto it = flags.find(name);
        if (it == flags.end())
            return empty;
        return it->second;
    }
};

/* ============================================================
   Base struct for command
   ============================================================ */

struct cvar_t
{
    std::string name;
    std::string description;

    cvar_t(const std::string& n, const std::string& d)
        : name(n), description(d)
    {
        registry()[name] = this;
    }

    virtual ~cvar_t() = default;

    virtual void exec(cctx_t& ctx) = 0;

    static std::map<std::string, cvar_t*>& registry()
    {
        static std::map<std::string, cvar_t*> inst;
        return inst;
    }
};

static void parse_line(const std::string& line,
    std::string& cmd,
    cctx_t& ctx)
{
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string t;

    while (iss >> t)
        tokens.push_back(t);

    if (tokens.empty())
        return;

    cmd = tokens[0];

    for (size_t i = 1; i < tokens.size(); ++i)
    {
        const std::string& tok = tokens[i];

        if (tok.rfind("--", 0) == 0)
        {
            std::string key = tok.substr(2);
            std::string val = "1";

            if (i + 1 < tokens.size() &&
                tokens[i + 1].rfind("--", 0) != 0)
            {
                val = tokens[i + 1];
                ++i;
            }

            ctx.flags[key] = val;
        }
        else
        {
            ctx.positional.push_back(tok);
        }
    }
}

/* ============================================================
   command help
   ============================================================ */

struct help_t : cvar_t
{
    help_t() : cvar_t("help", "show command list") {}

    void exec(cctx_t&) override
    {
        std::cout << "Command list:\n";
        for (const auto& kv : registry())
        {
            std::cout << "  " << kv.first
                << " � " << kv.second->description << "\n";
        }
    }
};

/*
 ============================================================
   user command example
 ============================================================ 
*/

struct send_t : cvar_t
{
    send_t() : cvar_t("send", "sending data (raw or packet format: send --cmd <code> <data>)") {}

    void exec(cctx_t& ctx) override
    {
        if (!ctx.tcp)
        {
            std::cout << ANSI_ERR << "TCP not initialized" << ANSI_ENDL << std::endl;
            return;
        }

        if (!ctx.tcp->is_connected())
        {
            std::cout << ANSI_WARN << "TCP not connected" << ANSI_ENDL << std::endl;
            return;
        }

        // Если указан флаг --cmd, отправляем пакет в формате CMD
        if (ctx.has_flag("cmd"))
        {
            std::string cmd_str = ctx.flag("cmd");
            cmdcode_t cmd_code = CMD_RESERVED;
            
            // Парсим код команды (может быть hex: 0x41 или decimal: 65)
            try
            {
                if (cmd_str.substr(0, 2) == "0x" || cmd_str.substr(0, 2) == "0X")
                {
                    cmd_code = static_cast<cmdcode_t>(std::stoul(cmd_str, nullptr, 16));
                }
                else
                {
                    cmd_code = static_cast<cmdcode_t>(std::stoul(cmd_str, nullptr, 10));
                }
            }
            catch (...)
            {
                std::cout << ANSI_ERR << "Invalid command code format. Use hex (0x41) or decimal (65)" << ANSI_ENDL << std::endl;
                return;
            }

            std::string data = ctx.get(0);
            if (data.empty())
            {
                std::cout << ANSI_WARN << "No data to send. Usage: send --cmd <code> <data>" << ANSI_ENDL << std::endl;
                return;
            }

            if (!ctx.tcp->send_packet(cmd_code, data))
            {
                std::cout << ANSI_ERR << "Packet send failed" << ANSI_ENDL << std::endl;
            }
            else
            {
                std::cout << ANSI_SUCC << "Packet sent: CMD=0x" << std::hex << cmd_code 
                          << std::dec << ", Data=" << data << ANSI_ENDL << std::endl;
            }
        }
        else
        {
            // Отправка raw данных (без упаковки)
            std::string data = ctx.get(0);
            if (data.empty())
            {
                std::cout << ANSI_WARN << "No data to send. Usage: send <data> or send --cmd <code> <data>" << ANSI_ENDL << std::endl;
                return;
            }

            if (!ctx.tcp->send(data))
            {
                std::cout << ANSI_ERR << "Send failed" << ANSI_ENDL << std::endl;
            }
            else
            {
                std::cout << ANSI_SUCC << "Data sent: " << data << ANSI_ENDL << std::endl;
            }
        }
    }
};

/* ============================================================
   command status
   ============================================================ */

struct status_t : cvar_t
{
    status_t() : cvar_t("status", "show WiFi and TCP status") {}

    void exec(cctx_t& ctx) override
    {
        std::cout << ANSI_INFO << "=== Status Information ===" << ANSI_ENDL << std::endl;
        
        // WiFi Status
        std::cout << ANSI_INFO << "WiFi:" << ANSI_ENDL;
        if (!ctx.wifi)
        {
            std::cout << ANSI_ERR << "  Status: NOT INITIALIZED" << ANSI_ENDL << std::endl;
        }
        else
        {
            wifi_state_t wifi_state = ctx.wifi->get_state();
            std::string wifi_state_str = ctx.wifi->get_state_string();
            bool wifi_connected = ctx.wifi->is_connected();
            
            // Цветовое оформление состояния WiFi
            std::string wifi_color;
            if (wifi_state == wifi_state_t::connected)
            {
                wifi_color = ANSI_SUCC;
            }
            else if (wifi_state == wifi_state_t::lost || wifi_state == wifi_state_t::retry)
            {
                wifi_color = ANSI_WARN;
            }
            else if (wifi_state == wifi_state_t::init)
            {
                wifi_color = ANSI_ERR;
            }
            else
            {
                wifi_color = ANSI_INFO;
            }
            
            std::cout << "  State: " << wifi_color << wifi_state_str << ANSI_ENDL;
            std::cout << "  Connected: " << (wifi_connected ? ANSI_SUCC : ANSI_WARN) 
                      << (wifi_connected ? "YES" : "NO") << ANSI_ENDL << std::endl;
        }
        
        // TCP Status
        std::cout << ANSI_INFO << "TCP:" << ANSI_ENDL;
        if (!ctx.tcp)
        {
            std::cout << ANSI_ERR << "  Status: NOT INITIALIZED" << ANSI_ENDL << std::endl;
        }
        else
        {
            tcp_state_t tcp_state = ctx.tcp->get_state();
            std::string tcp_state_str = ctx.tcp->get_state_string();
            bool tcp_connected = ctx.tcp->is_connected();
            
            // Цветовое оформление состояния TCP
            std::string tcp_color;
            if (tcp_state == tcp_state_t::connected)
            {
                tcp_color = ANSI_SUCC;
            }
            else if (tcp_state == tcp_state_t::lost || tcp_state == tcp_state_t::retry)
            {
                tcp_color = ANSI_WARN;
            }
            else if (tcp_state == tcp_state_t::init)
            {
                tcp_color = ANSI_ERR;
            }
            else
            {
                tcp_color = ANSI_INFO;
            }
            
            std::cout << "  State: " << tcp_color << tcp_state_str << ANSI_ENDL;
            std::cout << "  Connected: " << (tcp_connected ? ANSI_SUCC : ANSI_WARN) 
                      << (tcp_connected ? "YES" : "NO") << ANSI_ENDL << std::endl;
        }
    }
};

/* ============================================================
   command test_esp32
   ============================================================ */

struct test_esp32_t : cvar_t
{
    test_esp32_t() : cvar_t("test_esp32", "test connection with ESP32 and get its status") {}

    void exec(cctx_t& ctx) override
    {
        if (!ctx.tcp)
        {
            std::cout << ANSI_ERR << "TCP not initialized" << ANSI_ENDL << std::endl;
            return;
        }

        if (!ctx.tcp->is_connected())
        {
            std::cout << ANSI_WARN << "TCP not connected. Cannot test ESP32." << ANSI_ENDL << std::endl;
            return;
        }

        std::cout << ANSI_INFO << "Testing connection with ESP32..." << ANSI_ENDL << std::endl;

        // Сбрасываем флаги
        g_test_response_received.store(false);
        g_status_response_received.store(false);
        g_esp32_status_response.clear();

        // Отправляем тестовый пакет
        if (!ctx.tcp->send_packet(CMD_WIN, "TEST"))
        {
            std::cout << ANSI_ERR << "Failed to send test packet" << ANSI_ENDL << std::endl;
            return;
        }

        std::cout << ANSI_INFO << "Test packet sent, waiting for response..." << ANSI_ENDL << std::endl;

        // Ожидаем ответ с таймаутом (2 секунды)
        {
            std::unique_lock<std::mutex> lock(g_response_mutex);
            if (g_response_cv.wait_for(lock, std::chrono::seconds(2), 
                [] { return g_test_response_received.load(); }))
            {
                std::cout << ANSI_SUCC << "Test response received successfully!" << ANSI_ENDL << std::endl;
            }
            else
            {
                std::cout << ANSI_WARN << "Test response timeout. ESP32 may not be connected or not responding." << ANSI_ENDL << std::endl;
                return;
            }
        }

        // Если тест успешен, запрашиваем статус
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Небольшая задержка
        
        std::cout << ANSI_INFO << "Requesting status from ESP32..." << ANSI_ENDL << std::endl;
        
        if (!ctx.tcp->send_packet(CMD_WIN, "STATUS"))
        {
            std::cout << ANSI_ERR << "Failed to send status request" << ANSI_ENDL << std::endl;
            return;
        }

        // Ожидаем ответ со статусом с таймаутом (2 секунды)
        {
            std::unique_lock<std::mutex> lock(g_response_mutex);
            if (g_response_cv.wait_for(lock, std::chrono::seconds(2), 
                [] { return g_status_response_received.load(); }))
            {
                // Парсим и выводим статус
                std::string status = g_esp32_status_response;
                std::cout << ANSI_SUCC << "=== ESP32 Status ===" << ANSI_ENDL << std::endl;
                
                // Парсим формат: STATUS:WIFI=<state>:<connected>,TCP=<state>:<connected>
                if (status.length() > 7 && status.substr(0, 7) == "STATUS:")
                {
                    std::string status_data = status.substr(7);
                    
                    // Парсим WiFi статус
                    size_t wifi_pos = status_data.find("WIFI=");
                    size_t tcp_pos = status_data.find(",TCP=");
                    
                    if (wifi_pos != std::string::npos && tcp_pos != std::string::npos)
                    {
                        // WiFi
                        std::string wifi_info = status_data.substr(wifi_pos + 5, tcp_pos - wifi_pos - 5);
                        size_t colon_pos = wifi_info.find(':');
                        if (colon_pos != std::string::npos)
                        {
                            std::string wifi_state = wifi_info.substr(0, colon_pos);
                            std::string wifi_connected = wifi_info.substr(colon_pos + 1);
                            
                            std::cout << ANSI_INFO << "WiFi:" << ANSI_ENDL;
                            std::string wifi_color = (wifi_state == "CONNECTED") ? ANSI_SUCC : 
                                                    ((wifi_state == "LOST" || wifi_state == "RETRY") ? ANSI_WARN : ANSI_INFO);
                            std::cout << "  State: " << wifi_color << wifi_state << ANSI_ENDL;
                            std::cout << "  Connected: " << (wifi_connected == "1" ? ANSI_SUCC : ANSI_WARN)
                                      << (wifi_connected == "1" ? "YES" : "NO") << ANSI_ENDL << std::endl;
                        }
                        
                        // TCP
                        std::string tcp_info = status_data.substr(tcp_pos + 5);
                        colon_pos = tcp_info.find(':');
                        if (colon_pos != std::string::npos)
                        {
                            std::string tcp_state = tcp_info.substr(0, colon_pos);
                            std::string tcp_connected = tcp_info.substr(colon_pos + 1);
                            
                            std::cout << ANSI_INFO << "TCP:" << ANSI_ENDL;
                            std::string tcp_color = (tcp_state == "CONNECTED") ? ANSI_SUCC : 
                                                    ((tcp_state == "LOST" || tcp_state == "RETRY") ? ANSI_WARN : ANSI_INFO);
                            std::cout << "  State: " << tcp_color << tcp_state << ANSI_ENDL;
                            std::cout << "  Connected: " << (tcp_connected == "1" ? ANSI_SUCC : ANSI_WARN)
                                      << (tcp_connected == "1" ? "YES" : "NO") << ANSI_ENDL << std::endl;
                        }
                    }
                }
                else
                {
                    std::cout << ANSI_WARN << "Received status in unexpected format: " << status << ANSI_ENDL << std::endl;
                }
            }
            else
            {
                std::cout << ANSI_WARN << "Status response timeout." << ANSI_ENDL << std::endl;
            }
        }
    }
};


#endif
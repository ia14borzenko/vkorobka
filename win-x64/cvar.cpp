#include "cvar.hpp"
#include "message_protocol.h"

const std::string& cctx_t::get(size_t index) const
{
    static const std::string empty;
    if (index >= positional.size())
        return empty;
    return positional[index];
}

bool cctx_t::has_flag(const std::string& name) const
{
    return flags.find(name) != flags.end();
}

const std::string& cctx_t::flag(const std::string& name) const
{
    static const std::string empty;
    auto it = flags.find(name);
    if (it == flags.end())
        return empty;
    return it->second;
}

cvar_t::cvar_t(const std::string& n, const std::string& d)
    : name(n), description(d)
{
    cvar_t::registry()[name] = this;
}

std::map<std::string, cvar_t*>& cvar_t::registry()
{
    static std::map<std::string, cvar_t*> inst;
    return inst;
}

void parse_line(const std::string& line,
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

help_t::help_t() : cvar_t("help", "show command list") {}

void help_t::exec(cctx_t&)
{
    std::cout << "Command list:\n";
    for (const auto& kv : cvar_t::registry())
    {
        std::cout << "  " << kv.first
            << " — " << kv.second->description << "\n";
    }
}

send_t::send_t() : cvar_t("send", "sending data via new protocol (send <destination> <data> or send --type <type> <destination> <data>)") {}

void send_t::exec(cctx_t& ctx)
{
    if (!ctx.message_router)
    {
        std::cout << ANSI_ERR << "Message router not initialized" << ANSI_ENDL << std::endl;
        return;
    }

    if (!ctx.tcp || !ctx.tcp->is_connected())
    {
        std::cout << ANSI_WARN << "TCP not connected" << ANSI_ENDL << std::endl;
        return;
    }

    // Определяем тип сообщения и получателя
    msg_type_t msg_type = MSG_TYPE_COMMAND;
    msg_destination_t destination = MSG_DST_ESP32;
    
    if (ctx.has_flag("type"))
    {
        std::string type_str = ctx.flag("type");
        if (type_str == "command" || type_str == "cmd")
            msg_type = MSG_TYPE_COMMAND;
        else if (type_str == "data")
            msg_type = MSG_TYPE_DATA;
        else if (type_str == "stream")
            msg_type = MSG_TYPE_STREAM;
        else
        {
            std::cout << ANSI_ERR << "Invalid message type. Use: command, data, stream" << ANSI_ENDL << std::endl;
            return;
        }
    }

    // Определяем получателя
    std::string dest_str = ctx.get(0);
    if (dest_str == "esp32" || dest_str == "esp")
        destination = MSG_DST_ESP32;
    else if (dest_str == "stm32" || dest_str == "stm")
        destination = MSG_DST_STM32;
    else if (dest_str == "win" || dest_str == "windows")
        destination = MSG_DST_WIN;
    else if (!dest_str.empty())
    {
        std::cout << ANSI_ERR << "Invalid destination. Use: esp32, stm32, win" << ANSI_ENDL << std::endl;
        return;
    }

    // Получаем данные для отправки
    std::string data = ctx.has_flag("type") ? ctx.get(2) : ctx.get(1);
    if (data.empty())
    {
        std::cout << ANSI_WARN << "No data to send. Usage: send <destination> <data> or send --type <type> <destination> <data>" << ANSI_ENDL << std::endl;
        return;
    }

    // Создаем заголовок сообщения
    msg_header_t header = msg_create_header(
        msg_type,
        MSG_SRC_WIN,
        destination,
        128,  // priority
        0,    // stream_id
        static_cast<u32>(data.size()),
        0,    // sequence
        MSG_ROUTE_NONE
    );

    // Отправляем через message_router
    if (ctx.message_router->send_message(header, reinterpret_cast<const u8*>(data.c_str()), static_cast<u32>(data.size())))
    {
        std::cout << ANSI_SUCC << "Message sent: Type=" << (int)msg_type 
                  << ", Destination=" << (int)destination 
                  << ", Data=" << data << ANSI_ENDL << std::endl;
    }
    else
    {
        std::cout << ANSI_ERR << "Message send failed" << ANSI_ENDL << std::endl;
    }
}

status_t::status_t() : cvar_t("status", "show WiFi and TCP status") {}

void status_t::exec(cctx_t& ctx)
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

test_esp32_t::test_esp32_t() : cvar_t("test_esp32", "test connection with ESP32 and get its status") {}

void test_esp32_t::exec(cctx_t& ctx)
{
    if (!ctx.message_router)
    {
        std::cout << ANSI_ERR << "Message router not initialized" << ANSI_ENDL << std::endl;
        return;
    }

    if (!ctx.tcp || !ctx.tcp->is_connected())
    {
        std::cout << ANSI_WARN << "TCP not connected. Cannot test ESP32." << ANSI_ENDL << std::endl;
        return;
    }

    std::cout << ANSI_INFO << "Testing connection with ESP32..." << ANSI_ENDL << std::endl;

    // Сбрасываем флаги
    g_test_response_received.store(false);
    g_status_response_received.store(false);
    g_esp32_status_response.clear();

    // Отправляем тестовое сообщение через новый протокол
    std::string test_data = "TEST";
    msg_header_t test_header = msg_create_header(
        MSG_TYPE_COMMAND,
        MSG_SRC_WIN,
        MSG_DST_ESP32,
        128,  // priority
        0,    // stream_id
        static_cast<u32>(test_data.size()),
        0,    // sequence
        MSG_ROUTE_NONE
    );

    if (!ctx.message_router->send_message(test_header, reinterpret_cast<const u8*>(test_data.c_str()), static_cast<u32>(test_data.size())))
    {
        std::cout << ANSI_ERR << "Failed to send test message" << ANSI_ENDL << std::endl;
        return;
    }

    std::cout << ANSI_INFO << "Test message sent, waiting for response..." << ANSI_ENDL << std::endl;

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
    
    std::string status_data = "STATUS";
    msg_header_t status_header = msg_create_header(
        MSG_TYPE_COMMAND,
        MSG_SRC_WIN,
        MSG_DST_ESP32,
        128,  // priority
        0,    // stream_id
        static_cast<u32>(status_data.size()),
        0,    // sequence
        MSG_ROUTE_NONE
    );

    if (!ctx.message_router->send_message(status_header, reinterpret_cast<const u8*>(status_data.c_str()), static_cast<u32>(status_data.size())))
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
}

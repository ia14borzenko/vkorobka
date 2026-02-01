
#include <ws2tcpip.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "sys.hpp"
#include "netpack.h"


char rx_payload[10240];

static help_t help_cmd;
static send_t send_cmd;
static status_t status_cmd;
static test_esp32_t test_esp32_cmd;

static std::atomic<bool> g_running{ true };

// Глобальные указатели на TCP и WiFi объекты для доступа из console_thread
static tcp_t* g_tcp_ptr = nullptr;
static wifi_t* g_wifi_ptr = nullptr;

void console_thread()
{
    std::cout << "'help' for command list\n" << ANSI_ENDL;

    while (g_running.load())
    {
        std::cout << "> ";

        std::string line;
        if (!std::getline(std::cin, line))
        {
            g_running.store(false);
            break;
        }

        std::string cmd_name;
        cctx_t ctx;
        ctx.tcp = g_tcp_ptr;  // Передаем указатель на TCP объект
        ctx.wifi = g_wifi_ptr;  // Передаем указатель на WiFi объект

        parse_line(line, cmd_name, ctx);

        if (cmd_name.empty())
            continue;

        auto& reg = cvar_t::registry();
        auto it = reg.find(cmd_name);
        if (it == reg.end())
        {
            std::cout << ANSI_WARN << "unknown command" << ANSI_ENDL << std::endl;
            continue;
        }

        it->second->exec(ctx);
    }
}

static tcp_t tcp(&g_running);
static wifi_t wifi(&g_running);

// Глобальные переменные для синхронизации с командой test_esp32
std::mutex g_response_mutex;
std::condition_variable g_response_cv;
std::atomic<bool> g_test_response_received{ false };
std::atomic<bool> g_status_response_received{ false };
std::string g_esp32_status_response;

// Обработчик принятых пакетов
void handle_packet(cmdcode_t cmd_code, const char* payload, u32 payload_len)
{
    std::cout << ANSI_INFO << "[app] Packet received:" << ANSI_ENDL;
    std::cout << "  CMD Code: 0x" << std::hex << cmd_code << std::dec << std::endl;
    std::cout << "  Payload length: " << payload_len << " bytes" << std::endl;
    
    if (payload_len > 0 && payload != nullptr)
    {
        std::cout << "  Payload: ";
        // Выводим данные (первые 100 байт для читаемости)
        size_t print_len = (payload_len > 100) ? 100 : payload_len;
        for (u32 i = 0; i < print_len; ++i)
        {
            if (payload[i] >= 32 && payload[i] < 127)
            {
                std::cout << payload[i];
            }
            else
            {
                std::cout << "\\x" << std::hex << (unsigned char)payload[i] << std::dec;
            }
        }
        if (payload_len > 100)
        {
            std::cout << "...";
        }
        std::cout << std::endl;
    }
    
    // Обработка специальных команд от ESP32
    if (cmd_code == CMD_ESP && payload_len > 0 && payload != nullptr)
    {
        std::string payload_str(payload, payload_len);
        
        // Проверяем, является ли это ответом на тест
        if (payload_str == "TEST_RESPONSE")
        {
            std::lock_guard<std::mutex> lock(g_response_mutex);
            g_test_response_received.store(true);
            g_response_cv.notify_one();
            std::cout << ANSI_SUCC << "[app] Test response received from ESP32" << ANSI_ENDL << std::endl;
            return;
        }
        
        // Проверяем, является ли это ответом со статусом
        if (payload_str.substr(0, 7) == "STATUS:")
        {
            std::lock_guard<std::mutex> lock(g_response_mutex);
            g_esp32_status_response = payload_str;
            g_status_response_received.store(true);
            g_response_cv.notify_one();
            std::cout << ANSI_SUCC << "[app] Status response received from ESP32" << ANSI_ENDL << std::endl;
            return;
        }
    }
    
    // Здесь можно добавить обработку различных типов пакетов
    switch (cmd_code)
    {
    case CMD_ESP:
        std::cout << ANSI_SUCC << "  Type: ESP32 command" << ANSI_ENDL << std::endl;
        break;
    case CMD_STM:
        std::cout << ANSI_SUCC << "  Type: STM32 command" << ANSI_ENDL << std::endl;
        break;
    case CMD_MIC:
        std::cout << ANSI_SUCC << "  Type: Microphone data" << ANSI_ENDL << std::endl;
        break;
    case CMD_SPK:
        std::cout << ANSI_SUCC << "  Type: Speaker data" << ANSI_ENDL << std::endl;
        break;
    case CMD_RESERVED:
        std::cout << ANSI_SUCC << "  Type: Raw data (not CMD format)" << ANSI_ENDL << std::endl;
        break;
    default:
        if (cmd_code != CMD_RESERVED)
        {
            std::cout << ANSI_SUCC << "  Type: Unknown command code" << ANSI_ENDL << std::endl;
        }
        break;
    }
}

int main()
{
    g_tcp_ptr = &tcp;  // Устанавливаем глобальный указатель на TCP
    g_wifi_ptr = &wifi;  // Устанавливаем глобальный указатель на WiFi

    // Устанавливаем обработчик принятых пакетов
    tcp.set_packet_callback(handle_packet);

    std::thread t_console(console_thread);

    // Запускаем WiFi
    if (!wifi.start())
    {
        std::cout << ANSI_ERR << "[wifi] WiFi start failed" << ANSI_ENDL << std::endl;
        g_running.store(false);
        t_console.join();
        return 1;
    }

    // Запускаем TCP
    if (!tcp.start())
    {
        std::cout << ANSI_ERR << "[tcp] TCP start failed" << ANSI_ENDL << std::endl;
        g_running.store(false);
        wifi.stop();
        t_console.join();
        return 1;
    }

    std::cout << ANSI_SUCC << "[main] WiFi and TCP servers started. Waiting for connections..." << ANSI_ENDL << std::endl;

    // Основной цикл - просто ждем завершения
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    wifi.stop();
    tcp.stop();
    t_console.join();

    return 0;
}

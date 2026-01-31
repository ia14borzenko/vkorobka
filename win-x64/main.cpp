
#include <ws2tcpip.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <string>

#include "sys.hpp"
#include "netpack.h"


char rx_payload[10240];

static help_t help_cmd;
static send_t send_cmd;

static std::atomic<bool> g_running{ true };

// Глобальный указатель на TCP объект для доступа из console_thread
static tcp_t* g_tcp_ptr = nullptr;

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
    default:
        break;
    }
}

int main()
{
    g_tcp_ptr = &tcp;  // Устанавливаем глобальный указатель

    // Устанавливаем обработчик принятых пакетов
    tcp.set_packet_callback(handle_packet);

    std::thread t_console(console_thread);

    if (!tcp.start())
    {
        std::cout << ANSI_ERR << "[tcp] TCP start failed" << ANSI_ENDL << std::endl;
        g_running.store(false);
        t_console.join();
        return 1;
    }

    std::cout << ANSI_SUCC << "[main] TCP server started. Waiting for connections..." << ANSI_ENDL << std::endl;

    // Основной цикл - просто ждем завершения
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    tcp.stop();
    t_console.join();

    return 0;
}

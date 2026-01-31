#ifndef WIFI_HPP
#define WIFI_HPP

#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

static std::atomic<bool> g_wifi_up{ false };

enum class wifi_state_t
{
    init,
    listening,
    connected,
    lost,
    retry
};

class wifi_t
{
public:
    wifi_t(std::atomic<bool>* g_running_p, int port = 1235) 
        : g_running(g_running_p)
        , listen_sock(INVALID_SOCKET)
        , client_sock(INVALID_SOCKET)
        , current_state(wifi_state_t::init)
        , listen_port(port)
    {}

    ~wifi_t()
    {
        stop();
    }

    bool start(void);

    void stop(void);

    SOCKET get_client_socket(void) const;

    bool is_connected(void) const;
    
    // Получить текущее состояние WiFi
    wifi_state_t get_state(void) const;
    
    // Получить строковое представление состояния
    std::string get_state_string(void) const;
    
    // Принудительное переподключение (сброс состояния)
    void force_reconnect(void);

private:
    SOCKET listen_sock;
    SOCKET client_sock;
    std::thread worker;
    std::atomic<bool>* g_running;
    std::atomic<wifi_state_t> current_state;  // Текущее состояние для мониторинга
    int listen_port;  // Порт для прослушивания

    int wifi_init(void);

    bool start_listening(void);

    bool accept_connection(void);

    bool connection_alive(void) const;

    void close_all(void);

    void run(void);
};

#endif // WIFI_HPP
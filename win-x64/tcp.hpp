#ifndef TCP_HPP
#define TCP_HPP

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>  // Для TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <functional>
#include <cstring>

// Include netpack for packet format
#include "netpack.h"
#include "my_types.h"

#pragma comment(lib, "Ws2_32.lib")

enum class tcp_state_t
{
    init,
    listening,
    connected,
    lost,
    retry
};

class tcp_t
{
public:
    // Callback для обработки принятых пакетов
    // Параметры: cmd_code - код команды, payload - данные пакета, payload_len - длина данных
    using packet_callback_t = std::function<void(cmdcode_t cmd_code, const char* payload, u32 payload_len)>;

    tcp_t(std::atomic<bool>* g_running_p);
    ~tcp_t();

    // Запуск TCP сервера (поток для восстановления соединения)
    bool start();
    void stop();

    SOCKET get_client_socket() const;
    bool is_connected() const;

    // Получить текущее состояние TCP
    tcp_state_t get_state() const;

    // Получить строковое представление состояния
    std::string get_state_string() const;

    // Установка callback для обработки принятых пакетов
    void set_packet_callback(packet_callback_t callback);

    // Отправка данных через TCP (raw, без упаковки)
    bool send(const std::string& data);
    bool send(const char* data, int len);

    // Отправка пакета в формате CMD (с автоматической упаковкой)
    bool send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len);
    // Удобная перегрузка для отправки строки
    bool send_packet(cmdcode_t cmd_code, const std::string& payload);

private:
    std::atomic<bool>* g_running;
    SOCKET listen_sock;
    SOCKET client_sock;
    std::thread worker;
    std::atomic<bool> tcp_connected;
    std::atomic<tcp_state_t> current_state;  // Текущее состояние для мониторинга
    packet_callback_t rx_callback;
    
    // Буфер для накопления входящих данных
    std::vector<char> rx_buffer;

    int tcp_init();
    bool start_listening();
    bool accept_connection();
    void read_initial_data();
    void read_and_process_data();
    void handle_connection_error();
    bool connection_alive() const;
    void close_all();
    void run();
    void process_rx_buffer();
};

#endif // TCP_HPP

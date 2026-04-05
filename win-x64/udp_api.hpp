#ifndef UDP_API_HPP
#define UDP_API_HPP

#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "message_protocol.h"

#pragma comment(lib, "Ws2_32.lib")

// Callback для обработки входящих JSON сообщений
using udp_json_callback_t = std::function<void(const std::string& json_str, const std::string& client_ip, int client_port)>;

class udp_api_t
{
public:
    udp_api_t(std::atomic<bool>* g_running_p, int port = 1236);
    ~udp_api_t();

    // Запуск UDP сервера
    bool start();
    void stop();

    // Отправка JSON сообщения на указанный адрес (при ошибке в *out_wsa_error — код WSA, иначе не трогать)
    bool send_json_to(const std::string& ip, int port, const std::string& json_str, int* out_wsa_error = nullptr);

    // Отправка JSON сообщения всем зарегистрированным клиентам
    bool send_json_to_all(const std::string& json_str);

    // Регистрация клиента (автоматически при получении сообщения)
    void register_client(const std::string& ip, int port);

    // Установка callback для обработки входящих JSON сообщений
    void set_json_callback(udp_json_callback_t callback);

    int get_port() const { return listen_port; }

private:
    std::atomic<bool>* g_running;
    SOCKET sock;
    std::thread worker;
    int listen_port;
    udp_json_callback_t json_callback;
    
    // Регистрация клиентов (IP:port -> sockaddr_in)
    std::map<std::string, sockaddr_in> clients_;
    std::mutex clients_mutex_;
    
    int udp_init();
    void close_socket();
    void run();
    std::string get_client_key(const std::string& ip, int port);
};

#endif // UDP_API_HPP

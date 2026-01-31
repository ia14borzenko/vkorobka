#ifndef TCP_HPP
#define TCP_HPP

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
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

    tcp_t(std::atomic<bool>* g_running_p)
        : g_running(g_running_p)
        , listen_sock(INVALID_SOCKET)
        , client_sock(INVALID_SOCKET)
        , tcp_connected(false)
        , rx_callback(nullptr)
    {
    }

    ~tcp_t()
    {
        stop();
    }

    // Запуск TCP сервера (поток для восстановления соединения)
    bool start()
    {
        if (tcp_init() != 0)
        {
            return false;
        }

        worker = std::thread(&tcp_t::run, this);
        return true;
    }

    void stop()
    {
        if (g_running)
        {
            g_running->store(false);
        }

        if (worker.joinable())
        {
            worker.join();
        }

        close_all();
    }

    SOCKET get_client_socket() const
    {
        return client_sock;
    }

    bool is_connected() const
    {
        return tcp_connected.load();
    }

    // Установка callback для обработки принятых пакетов
    void set_packet_callback(packet_callback_t callback)
    {
        rx_callback = callback;
    }

    // Отправка данных через TCP (raw, без упаковки)
    bool send(const std::string& data)
    {
        return send(data.c_str(), static_cast<int>(data.size()));
    }

    bool send(const char* data, int len)
    {
        if (!is_connected() || client_sock == INVALID_SOCKET)
        {
            std::cerr << "[tcp] Cannot send: not connected\n";
            return false;
        }

        int sent = ::send(client_sock, data, len, 0);
        if (sent == SOCKET_ERROR)
        {
            std::cerr << "[tcp] Send failed: " << WSAGetLastError() << "\n";
            return false;
        }

        return sent == len;
    }

    // Отправка пакета в формате CMD (с автоматической упаковкой)
    bool send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len)
    {
        if (!is_connected() || client_sock == INVALID_SOCKET)
        {
            std::cerr << "[tcp] Cannot send packet: not connected\n";
            return false;
        }

        // Выделяем буфер для пакета
        std::vector<char> packet(CMD_HEADER_LEN + payload_len);
        
        // Упаковываем пакет
        u32 packet_size = pack_packet(cmd_code, payload, payload_len, packet.data());
        if (packet_size == 0)
        {
            std::cerr << "[tcp] Failed to pack packet\n";
            return false;
        }

        // Отправляем все данные (может потребоваться несколько вызовов send)
        const char* ptr = packet.data();
        int remaining = static_cast<int>(packet_size);
        
        while (remaining > 0)
        {
            int sent = ::send(client_sock, ptr, remaining, 0);
            if (sent == SOCKET_ERROR)
            {
                std::cerr << "[tcp] Send packet failed: " << WSAGetLastError() << "\n";
                return false;
            }
            if (sent == 0)
            {
                std::cerr << "[tcp] Connection closed during send\n";
                return false;
            }
            
            ptr += sent;
            remaining -= sent;
        }

        return true;
    }

    // Удобная перегрузка для отправки строки
    bool send_packet(cmdcode_t cmd_code, const std::string& payload)
    {
        return send_packet(cmd_code, payload.c_str(), static_cast<u32>(payload.size()));
    }

private:
    std::atomic<bool>* g_running;
    SOCKET listen_sock;
    SOCKET client_sock;
    std::thread worker;
    std::atomic<bool> tcp_connected;
    packet_callback_t rx_callback;
    
    // Буфер для накопления входящих данных
    std::vector<char> rx_buffer;

    int tcp_init()
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            std::cerr << "[tcp] WSAStartup failed: " << WSAGetLastError() << "\n";
            return -1;
        }

        listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == INVALID_SOCKET)
        {
            std::cerr << "[tcp] socket() failed: " << WSAGetLastError() << "\n";
            WSACleanup();
            return -1;
        }

        char opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        return 0;
    }

    bool start_listening()
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1234);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        {
            std::cerr << "[tcp] bind failed: " << WSAGetLastError() << "\n";
            return false;
        }

        if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR)
        {
            std::cerr << "[tcp] listen failed: " << WSAGetLastError() << "\n";
            return false;
        }

        std::cout << "[tcp] Listening on port 1234...\n";
        return true;
    }

    bool accept_connection()
    {
        sockaddr_in client_addr{};
        int len = sizeof(client_addr);

        client_sock = accept(listen_sock, (sockaddr*)&client_addr, &len);
        if (client_sock == INVALID_SOCKET)
        {
            std::cerr << "[tcp] accept failed: " << WSAGetLastError() << "\n";
            return false;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        std::cout << "[tcp] Connected: " << ip << ":" << ntohs(client_addr.sin_port) << "\n";
        return true;
    }

    bool connection_alive() const
    {
        if (client_sock == INVALID_SOCKET) return false;

        char tmp;
        int ret = recv(client_sock, &tmp, 1, MSG_PEEK);

        if (ret == 0) return false;                    // graceful close
        if (ret == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            return (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
        }

        return true;
    }

    void close_all()
    {
        if (client_sock != INVALID_SOCKET)
        {
            shutdown(client_sock, SD_BOTH);
            closesocket(client_sock);
            client_sock = INVALID_SOCKET;
        }

        if (listen_sock != INVALID_SOCKET)
        {
            closesocket(listen_sock);
            listen_sock = INVALID_SOCKET;
        }

        tcp_connected.store(false);
    }

    void run()
    {
        tcp_state_t state = tcp_state_t::init;

        while (g_running && g_running->load())
        {
            switch (state)
            {
            case tcp_state_t::init:
            {
                if (tcp_init() == 0)
                {
                    state = tcp_state_t::listening;
                }
                else
                {
                    state = tcp_state_t::retry;
                }
                break;
            }

            case tcp_state_t::listening:
            {
                if (start_listening())
                {
                    state = tcp_state_t::connected;
                }
                else
                {
                    state = tcp_state_t::retry;
                }
                break;
            }

            case tcp_state_t::connected:
            {
                if (accept_connection())
                {
                    tcp_connected.store(true);
                    rx_buffer.clear();  // Очищаем буфер при новом соединении
                    
                    // Устанавливаем сокет в неблокирующий режим для чтения
                    u_long mode = 1;
                    ioctlsocket(client_sock, FIONBIO, &mode);
                    
                    char rx_raw[4096];  // Буфер для чтения из сокета
                    
                    while (g_running && g_running->load() && connection_alive())
                    {
                        // Читаем входящие данные
                        int len = recv(client_sock, rx_raw, sizeof(rx_raw), 0);
                        
                        if (len > 0)
                        {
                            // Добавляем данные в буфер
                            rx_buffer.insert(rx_buffer.end(), rx_raw, rx_raw + len);
                            
                            // Обрабатываем все полные пакеты из буфера
                            process_rx_buffer();
                        }
                        else if (len == 0)
                        {
                            // Соединение закрыто клиентом
                            std::cout << "[tcp] Connection closed by client\n";
                            break;
                        }
                        else
                        {
                            // Ошибка или нет данных (WSAEWOULDBLOCK)
                            int err = WSAGetLastError();
                            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
                            {
                                std::cerr << "[tcp] recv error: " << err << "\n";
                                break;
                            }
                        }
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    
                    tcp_connected.store(false);
                    rx_buffer.clear();
                    state = tcp_state_t::lost;
                }
                else
                {
                    state = tcp_state_t::retry;
                }
                break;
            }

            case tcp_state_t::lost:
            {
                close_all();
                state = tcp_state_t::retry;
                break;
            }

            case tcp_state_t::retry:
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                state = tcp_state_t::init;
                break;
            }
            }
        }

        close_all();
        std::cout << "[tcp] Server thread stopped\n";
    }

    // Обработка буфера входящих данных - извлечение полных пакетов
    void process_rx_buffer()
    {
        while (rx_buffer.size() >= CMD_HEADER_LEN)
        {
            // Проверяем, есть ли полный заголовок
            const char* msg = rx_buffer.data();
            u32 msg_len = static_cast<u32>(rx_buffer.size());
            
            // Сначала читаем заголовок для проверки длины (читаем байты напрямую для правильного порядка байт)
            // CMD Code: 2 байта (little-endian)
            // Payload length: 4 байта (little-endian)
            cmdcode_t cmd_code_raw = static_cast<cmdcode_t>(msg[0] | (msg[1] << 8));
            u32 payload_len_raw = static_cast<u32>(msg[2] | (msg[3] << 8) | (msg[4] << 16) | (msg[5] << 24));
            
            // Проверка на разумность payload_len перед дальнейшей обработкой
            const u32 MAX_REASONABLE_PAYLOAD = 65535;
            if (payload_len_raw > MAX_REASONABLE_PAYLOAD)
            {
                // payload_len явно невалидный, пропускаем один байт
                std::cerr << "[tcp] Invalid payload length: " << payload_len_raw << ", skipping byte\n";
                rx_buffer.erase(rx_buffer.begin());
                continue;
            }
            
            u32 expected_total = CMD_HEADER_LEN + payload_len_raw;
            
            // Проверяем, есть ли полный пакет
            if (msg_len < expected_total)
            {
                // Недостаточно данных для полного пакета, ждем еще
                break;
            }
            
            // Пытаемся распарсить пакет
            const char* payload_begin = nullptr;
            u32 payload_len = 0;
            cmdcode_t cmd_code = is_pack(msg, expected_total, &payload_begin, &payload_len);
            
            if (cmd_code != CMD_RESERVED)
            {
                // Полный пакет получен и распарсен
                // Вызываем callback с данными пакета
                if (rx_callback)
                {
                    if (payload_len > 0 && payload_begin != nullptr)
                    {
                        rx_callback(cmd_code, payload_begin, payload_len);
                    }
                    else
                    {
                        // Пакет без данных
                        rx_callback(cmd_code, nullptr, 0);
                    }
                }
                
                // Удаляем обработанный пакет из буфера
                rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + expected_total);
            }
            else
            {
                // Неверный формат пакета, пропускаем один байт и пытаемся снова
                std::cerr << "[tcp] Invalid packet format, skipping byte\n";
                rx_buffer.erase(rx_buffer.begin());
            }
        }
    }
};

#endif // TCP_HPP

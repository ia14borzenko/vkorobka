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

    tcp_t(std::atomic<bool>* g_running_p)
        : g_running(g_running_p)
        , listen_sock(INVALID_SOCKET)
        , client_sock(INVALID_SOCKET)
        , tcp_connected(false)
        , current_state(tcp_state_t::init)
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

    // Получить текущее состояние TCP
    tcp_state_t get_state() const
    {
        return current_state.load();
    }

    // Получить строковое представление состояния
    std::string get_state_string() const
    {
        switch (current_state.load())
        {
        case tcp_state_t::init:
            return "INIT";
        case tcp_state_t::listening:
            return "LISTENING";
        case tcp_state_t::connected:
            return "CONNECTED";
        case tcp_state_t::lost:
            return "LOST";
        case tcp_state_t::retry:
            return "RETRY";
        default:
            return "UNKNOWN";
        }
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
        // Проверяем только наличие сокета и флаг соединения
        if (client_sock == INVALID_SOCKET || !tcp_connected.load())
        {
            std::cerr << "[tcp] Cannot send: not connected (socket: " << client_sock << ", connected: " << tcp_connected.load() << ")\n";
            return false;
        }

        // Дополнительная проверка: убеждаемся, что сокет действительно активен
        // Это важно, так как соединение может быть разорвано между проверкой и отправкой
        int error = 0;
        int error_len = sizeof(error);
        if (getsockopt(client_sock, SOL_SOCKET, SO_ERROR, (char*)&error, &error_len) == 0)
        {
            if (error != 0)
            {
                std::cerr << "[tcp] Socket has error " << error << " before send, closing connection\n";
                handle_connection_error();
                return false;
            }
        }

        std::cout << "[tcp] Sending " << len << " bytes to socket " << client_sock << "\n";
        int sent = ::send(client_sock, data, len, 0);
        if (sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            std::cerr << "[tcp] Send failed on socket " << client_sock << ": " << err << "\n";
            // Если ошибка критическая, обновляем состояние
            if (err == WSAENOTCONN || err == WSAECONNRESET || err == WSAECONNABORTED)
            {
                handle_connection_error();
            }
            return false;
        }

        std::cout << "[tcp] Successfully sent " << sent << " bytes\n";
        return sent == len;
    }

    // Отправка пакета в формате CMD (с автоматической упаковкой)
    bool send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len)
    {
        // Проверяем только наличие сокета и флаг соединения
        if (client_sock == INVALID_SOCKET || !tcp_connected.load())
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
                int err = WSAGetLastError();
                std::cerr << "[tcp] Send packet failed: " << err << "\n";
                // Если ошибка критическая, обновляем состояние
                if (err == WSAENOTCONN || err == WSAECONNRESET || err == WSAECONNABORTED)
                {
                    handle_connection_error();
                }
                return false;
            }
            if (sent == 0)
            {
                std::cerr << "[tcp] Connection closed during send\n";
                handle_connection_error();
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
    std::atomic<tcp_state_t> current_state;  // Текущее состояние для мониторинга
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
        
        // Устанавливаем listen_sock в неблокирующий режим для неблокирующего accept()
        u_long mode = 1;
        ioctlsocket(listen_sock, FIONBIO, &mode);

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
        // Закрываем старое соединение, если оно есть
        if (client_sock != INVALID_SOCKET)
        {
            std::cout << "[tcp] Closing old connection before accepting new one\n";
            closesocket(client_sock);
            client_sock = INVALID_SOCKET;
            tcp_connected.store(false);
        }
        
        sockaddr_in client_addr{};
        int len = sizeof(client_addr);

        // Блокирующий accept (вызывается только когда select() показал готовность)
        client_sock = accept(listen_sock, (sockaddr*)&client_addr, &len);
        if (client_sock == INVALID_SOCKET)
        {
            int err = WSAGetLastError();
            std::cerr << "[tcp] accept failed: " << err << "\n";
            return false;
        }

        // Включаем TCP keepalive для автоматического обнаружения разрыва
        BOOL keepalive = TRUE;
        if (setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, 
                      (char*)&keepalive, sizeof(keepalive)) == SOCKET_ERROR)
        {
            std::cerr << "[tcp] Failed to set SO_KEEPALIVE: " << WSAGetLastError() << "\n";
        }
        
        // Настраиваем параметры TCP keepalive для быстрого обнаружения разрыва (~5 секунд)
        // TCP_KEEPIDLE: время до первого keepalive probe (2 секунды)
        // TCP_KEEPINTVL: интервал между probes (1 секунда)
        // TCP_KEEPCNT: количество неудачных probes до разрыва (3)
        // Итого: 2 + 1*3 = 5 секунд до обнаружения разрыва
        DWORD keepidle = 2;      // 2 секунды до первого probe
        DWORD keepintvl = 1;     // 1 секунда между probes
        DWORD keepcount = 3;     // 3 неудачных probe = разрыв
        
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, 
                      (char*)&keepidle, sizeof(keepidle)) == SOCKET_ERROR)
        {
            std::cerr << "[tcp] Failed to set TCP_KEEPIDLE: " << WSAGetLastError() << "\n";
        }
        
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, 
                      (char*)&keepintvl, sizeof(keepintvl)) == SOCKET_ERROR)
        {
            std::cerr << "[tcp] Failed to set TCP_KEEPINTVL: " << WSAGetLastError() << "\n";
        }
        
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, 
                      (char*)&keepcount, sizeof(keepcount)) == SOCKET_ERROR)
        {
            std::cerr << "[tcp] Failed to set TCP_KEEPCNT: " << WSAGetLastError() << "\n";
        }
        
        std::cout << "[tcp] TCP keepalive configured: idle=2s, interval=1s, count=3 (~5s detection)\n";
        
        // Устанавливаем client_sock в неблокирующий режим
        u_long mode = 1;
        ioctlsocket(client_sock, FIONBIO, &mode);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

        std::cout << "[tcp] Connection accepted from " << ip << ":" << ntohs(client_addr.sin_port) << " (socket: " << client_sock << ")\n";
        
        tcp_connected.store(true);
        rx_buffer.clear();
        
        return true;
    }
    
    void read_initial_data()
    {
        std::cout << "[tcp] Reading initial data after accept (socket: " << client_sock << ")...\n";
        // Чтение всех доступных данных сразу после accept
        char initial_rx[4096];
        bool data_received = false;
        
        // Читаем в цикле пока есть данные (неблокирующий recv)
        while (true)
        {
            int initial_len = recv(client_sock, initial_rx, sizeof(initial_rx), 0);
            
            if (initial_len > 0)
            {
                std::cout << "[tcp] Received " << initial_len << " bytes (initial data after accept)\n";
                rx_buffer.insert(rx_buffer.end(), initial_rx, initial_rx + initial_len);
                data_received = true;
                // Продолжаем читать, пока есть данные
            }
            else if (initial_len == 0)
            {
                // Соединение закрыто сразу после accept
                std::cout << "[tcp] Connection closed immediately after accept\n";
                closesocket(client_sock);
                client_sock = INVALID_SOCKET;
                tcp_connected.store(false);
                return;
            }
            else
            {
                // WSAEWOULDBLOCK - данных больше нет
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                {
                    if (!data_received)
                    {
                        std::cout << "[tcp] No initial data available (WSAEWOULDBLOCK)\n";
                    }
                    break;
                }
                else
                {
                    std::cerr << "[tcp] Error reading initial data: " << err << "\n";
                    return;
                }
            }
        }
        
        // Обрабатываем начальные данные
        if (!rx_buffer.empty())
        {
            std::cout << "[tcp] Processing " << rx_buffer.size() << " bytes of initial data\n";
            process_rx_buffer();
        }
        else
        {
            std::cout << "[tcp] No initial data to process\n";
        }
    }
    
    void read_and_process_data()
    {
        // Чтение данных в цикле пока есть данные (неблокирующий recv)
        char rx_raw[4096];
        
        while (true)
        {
            int len = recv(client_sock, rx_raw, sizeof(rx_raw), 0);
            
            if (len > 0)
            {
                std::cout << "[tcp] Received " << len << " bytes from client\n";
                rx_buffer.insert(rx_buffer.end(), rx_raw, rx_raw + len);
                process_rx_buffer();
            }
            else if (len == 0)
            {
                // Соединение закрыто клиентом
                std::cout << "[tcp] Connection closed by client (recv returned 0)\n";
                handle_connection_error();
                return;
            }
            else
            {
                // WSAEWOULDBLOCK - данных больше нет
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK)
                {
                    // Все данные прочитаны
                    break;
                }
                else
                {
                    std::cerr << "[tcp] recv error: " << err << "\n";
                    handle_connection_error();
                    return;
                }
            }
        }
    }
    
    void handle_connection_error()
    {
        std::cout << "[tcp] Handling connection error, closing client socket\n";
        // Закрываем client_sock и возвращаемся к listening
        tcp_connected.store(false);
        rx_buffer.clear();
        
        if (client_sock != INVALID_SOCKET)
        {
            closesocket(client_sock);
            client_sock = INVALID_SOCKET;
        }
        
        std::cout << "[tcp] Client socket closed, returning to listening state\n";
        // listen_sock остается открытым для нового соединения
        // Состояние будет обновлено вызывающим кодом
    }

    bool connection_alive() const
    {
        if (client_sock == INVALID_SOCKET) return false;

        // Проверка 1: getsockopt(SO_ERROR) - проверяет ошибки на сокете
        int error = 0;
        int error_len = sizeof(error);
        if (getsockopt(client_sock, SOL_SOCKET, SO_ERROR, (char*)&error, &error_len) == 0)
        {
            if (error != 0)
            {
                // Есть ошибка на сокете - соединение разорвано
                return false;
            }
        }

        // Проверка 2: select() для проверки состояния сокета
        fd_set readfds, errorfds;
        FD_ZERO(&readfds);
        FD_ZERO(&errorfds);
        FD_SET(client_sock, &readfds);
        FD_SET(client_sock, &errorfds);
        
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        
        int ret = select(0, &readfds, nullptr, &errorfds, &timeout);
        
        if (ret == SOCKET_ERROR)
        {
            return false;  // Ошибка select
        }
        
        if (FD_ISSET(client_sock, &errorfds))
        {
            return false;  // Ошибка на сокете
        }
        
        // Проверка 3: recv(MSG_PEEK) для проверки закрытия соединения
        if (FD_ISSET(client_sock, &readfds))
        {
            char tmp;
            int recv_ret = recv(client_sock, &tmp, 1, MSG_PEEK);
            
            if (recv_ret == 0)
            {
                return false;  // Соединение закрыто
            }
            
            if (recv_ret == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
                {
                    return false;  // Реальная ошибка
                }
            }
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
        // Инициализация
        close_all();
        current_state.store(tcp_state_t::init);
        
        if (tcp_init() != 0)
        {
            std::cerr << "[tcp] Initialization failed\n";
            current_state.store(tcp_state_t::init);
            return;
        }
        
        if (!start_listening())
        {
            std::cerr << "[tcp] Failed to start listening\n";
            close_all();
            current_state.store(tcp_state_t::init);
            return;
        }
        
        current_state.store(tcp_state_t::listening);
        
        // Event-driven loop с select()
        fd_set readfds, errorfds;
        
        while (g_running && g_running->load())
        {
            FD_ZERO(&readfds);
            FD_ZERO(&errorfds);
            
            // Мониторим listen_sock (если нет клиента) или client_sock (если есть)
            if (client_sock == INVALID_SOCKET)
            {
                // Ожидаем новое соединение
                if (listen_sock != INVALID_SOCKET)
                {
                    FD_SET(listen_sock, &readfds);
                    FD_SET(listen_sock, &errorfds);
                }
            }
            else
            {
                // Ожидаем данные от клиента
                FD_SET(client_sock, &readfds);
                FD_SET(client_sock, &errorfds);
            }
            
            // Timeout для select (100ms)
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;
            
            int ret = select(0, &readfds, nullptr, &errorfds, &timeout);
            
            if (ret == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                std::cerr << "[tcp] select error: " << err << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            
            if (ret > 0)
            {
                // Есть события для обработки
                if (client_sock == INVALID_SOCKET)
                {
                    // Обработка listen_sock
                    if (FD_ISSET(listen_sock, &errorfds))
                    {
                        std::cerr << "[tcp] Error on listen socket\n";
                        // Переинициализация
                        close_all();
                        if (tcp_init() == 0 && start_listening())
                        {
                            continue;
                        }
                        else
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(3));
                            continue;
                        }
                    }
                    
                    if (FD_ISSET(listen_sock, &readfds))
                    {
                        // Готово к accept
                        std::cout << "[tcp] New connection pending, accepting...\n";
                        if (accept_connection())
                        {
                            current_state.store(tcp_state_t::connected);
                            read_initial_data();
                        }
                    }
                }
                else
                {
                    // Обработка client_sock
                    if (FD_ISSET(client_sock, &errorfds))
                    {
                        std::cout << "[tcp] Error detected on client socket\n";
                        current_state.store(tcp_state_t::lost);
                        handle_connection_error();
                        current_state.store(tcp_state_t::listening);
                    }
                    
                    if (FD_ISSET(client_sock, &readfds))
                    {
                        std::cout << "[tcp] Data available on client socket\n";
                        read_and_process_data();
                    }
                }
            }
            else if (ret == 0)
            {
                // Timeout - проверяем наличие данных и состояние соединения
                // Это важно, так как данные могут прийти между вызовами select()
                if (client_sock != INVALID_SOCKET && tcp_connected.load())
                {
                    // Периодическая проверка наличия данных и состояния соединения (каждые 5 циклов = ~0.5 секунды)
                    static int timeout_count = 0;
                    timeout_count++;
                    if (timeout_count >= 5)
                    {
                        timeout_count = 0;
                        
                        // Сначала проверяем состояние соединения
                        int error = 0;
                        int error_len = sizeof(error);
                        if (getsockopt(client_sock, SOL_SOCKET, SO_ERROR, (char*)&error, &error_len) == 0)
                        {
                        if (error != 0)
                        {
                            std::cout << "[tcp] Connection error detected during timeout check: " << error << "\n";
                            current_state.store(tcp_state_t::lost);
                            handle_connection_error();
                            current_state.store(tcp_state_t::listening);
                            continue;
                        }
                        }
                        
                        // Проверяем наличие данных через recv с MSG_PEEK
                        char tmp;
                        int peek_ret = recv(client_sock, &tmp, 1, MSG_PEEK);
                        if (peek_ret > 0)
                        {
                            // Есть данные, читаем их
                            std::cout << "[tcp] Data detected during timeout check, reading...\n";
                            read_and_process_data();
                        }
                        else if (peek_ret == 0)
                        {
                            // Соединение закрыто
                            std::cout << "[tcp] Connection closed detected during timeout check\n";
                            current_state.store(tcp_state_t::lost);
                            handle_connection_error();
                            current_state.store(tcp_state_t::listening);
                        }
                        // peek_ret < 0 с WSAEWOULDBLOCK - данных нет, это нормально
                    }
                }
            }
        }
        
        close_all();
        std::cout << "[tcp] Server thread stopped\n";
    }

    // Обработка буфера входящих данных - извлечение полных пакетов
    void process_rx_buffer()
    {
        u32 skip_count = 0;
        const u32 MAX_REASONABLE_PAYLOAD = 65535;
        const u32 MAX_SKIP_BEFORE_CLEAR = CMD_HEADER_LEN;
        const u32 MAX_RAW_DATA_SIZE = 1024;  // Максимальный размер raw данных для обработки
        
        // Логируем первые байты буфера для отладки
        if (!rx_buffer.empty())
        {
            std::cout << "[tcp] RX buffer contents (first " << (rx_buffer.size() > 16 ? 16 : rx_buffer.size()) << " bytes): ";
            for (size_t i = 0; i < (rx_buffer.size() > 16 ? 16 : rx_buffer.size()); ++i)
            {
                std::cout << (unsigned char)rx_buffer[i] << " " << std::dec;
            }
            std::cout << "\n";
        }
        
        // Если в буфере есть данные, но меньше CMD_HEADER_LEN, и буфер достаточно большой,
        // возможно это raw данные, которые нужно обработать и очистить
        if (rx_buffer.size() > 0 && rx_buffer.size() < CMD_HEADER_LEN && rx_buffer.size() <= MAX_RAW_DATA_SIZE)
        {
            // Пытаемся найти начало валидного CMD пакета, сканируя буфер
            bool found_valid_packet = false;
            size_t search_limit = (rx_buffer.size() > 100) ? 100 : rx_buffer.size();
            
            for (size_t offset = 0; offset <= search_limit && offset + CMD_HEADER_LEN <= rx_buffer.size(); ++offset)
            {
                const char* msg = rx_buffer.data() + offset;
                cmdcode_t cmd_code_raw = static_cast<cmdcode_t>(msg[0] | (msg[1] << 8));
                u32 payload_len_raw = static_cast<u32>(msg[2] | (msg[3] << 8) | (msg[4] << 16) | (msg[5] << 24));
                
                // Проверяем, является ли это валидным CMD кодом
                if ((cmd_code_raw == CMD_WIN || cmd_code_raw == CMD_ESP || cmd_code_raw == CMD_STM ||
                     cmd_code_raw == CMD_DPL || cmd_code_raw == CMD_MIC || cmd_code_raw == CMD_SPK) &&
                    payload_len_raw <= MAX_REASONABLE_PAYLOAD)
                {
                    // Нашли потенциально валидный заголовок, но данных недостаточно
                    // Обрабатываем данные до этого смещения как raw
                    if (offset > 0)
                    {
                        std::cout << "[tcp] Found potential CMD packet at offset " << offset 
                                  << ", treating " << offset << " bytes before as raw data\n";
                        
                        if (rx_callback)
                        {
                            rx_callback(CMD_RESERVED, rx_buffer.data(), static_cast<u32>(offset));
                        }
                        
                        rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + offset);
                    }
                    found_valid_packet = true;
                    break;
                }
            }
            
            // Если не нашли валидный пакет и буфер маленький, обрабатываем как raw
            if (!found_valid_packet && rx_buffer.size() <= MAX_RAW_DATA_SIZE)
            {
                std::cout << "[tcp] Small buffer (" << rx_buffer.size() 
                          << " bytes) doesn't contain valid CMD header, treating as raw data\n";
                
                if (rx_callback && !rx_buffer.empty())
                {
                    rx_callback(CMD_RESERVED, rx_buffer.data(), static_cast<u32>(rx_buffer.size()));
                }
                
                rx_buffer.clear();
                return;
            }
        }
        
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
            if (payload_len_raw > MAX_REASONABLE_PAYLOAD)
            {
                skip_count++;
                std::cerr << "[tcp] Invalid payload length: " << payload_len_raw 
                         << " (too large), skipping byte (skip_count=" << skip_count << ")\n";
                
                if (skip_count >= MAX_SKIP_BEFORE_CLEAR)
                {
                    std::cerr << "[tcp] Too many invalid bytes, treating as raw data and clearing\n";
                    
                    // Обрабатываем как raw данные перед очисткой
                    if (rx_callback && !rx_buffer.empty())
                    {
                        rx_callback(CMD_RESERVED, rx_buffer.data(), static_cast<u32>(rx_buffer.size()));
                    }
                    
                    rx_buffer.clear();
                    skip_count = 0;
                    break;
                }
                
                rx_buffer.erase(rx_buffer.begin());
                continue;
            }
            
            u32 expected_total = CMD_HEADER_LEN + payload_len_raw;
            
            std::cout << "[tcp] Checking packet header:\n";
            std::cout << "  CMD Code: 0x" << std::hex << cmd_code_raw << std::dec << "\n";
            std::cout << "  Payload length (from header): " << payload_len_raw << " bytes\n";
            std::cout << "  Expected total packet size: " << expected_total << " bytes\n";
            std::cout << "  Current buffer size: " << msg_len << " bytes\n";
            
            // Проверяем, есть ли полный пакет
            if (msg_len < expected_total)
            {
                // Недостаточно данных для полного пакета, ждем еще
                std::cout << "[tcp] Incomplete packet, waiting for more data (need " 
                         << (expected_total - msg_len) << " more bytes)\n";
                break;
            }
            
            // Пытаемся распарсить пакет
            const char* payload_begin = nullptr;
            u32 payload_len = 0;
            cmdcode_t cmd_code = is_pack(msg, expected_total, &payload_begin, &payload_len);
            
            if (cmd_code != CMD_RESERVED)
            {
                skip_count = 0;
                
                std::cout << "[tcp] CMD format check PASSED\n";
                std::cout << "  Valid CMD Code: 0x" << std::hex << cmd_code << std::dec << "\n";
                std::cout << "  Payload length: " << payload_len << " bytes\n";
                
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
                else
                {
                    std::cerr << "[tcp] WARNING: rx_callback is null!\n";
                }
                
                // Удаляем обработанный пакет из буфера
                rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + expected_total);
                continue;
            }
            else
            {
                skip_count++;
                std::cerr << "[tcp] Invalid packet format - CMD format check FAILED\n";
                std::cerr << "  Header CMD Code: 0x" << std::hex << cmd_code_raw << std::dec << "\n";
                std::cerr << "  Header Payload Length: " << payload_len_raw << "\n";
                
                // Если накопилось много пропусков, пытаемся найти начало валидного пакета
                if (skip_count >= MAX_SKIP_BEFORE_CLEAR)
                {
                    // Ищем начало валидного CMD пакета в буфере
                    bool found_valid = false;
                    size_t search_start = 1;
                    size_t search_limit = (rx_buffer.size() > 200) ? 200 : rx_buffer.size();
                    
                    for (size_t offset = search_start; offset + CMD_HEADER_LEN <= rx_buffer.size() && offset < search_limit; ++offset)
                    {
                        const char* test_msg = rx_buffer.data() + offset;
                        cmdcode_t test_cmd = static_cast<cmdcode_t>(test_msg[0] | (test_msg[1] << 8));
                        u32 test_len = static_cast<u32>(test_msg[2] | (test_msg[3] << 8) | (test_msg[4] << 16) | (test_msg[5] << 24));
                        
                        if ((test_cmd == CMD_WIN || test_cmd == CMD_ESP || test_cmd == CMD_STM ||
                             test_cmd == CMD_DPL || test_cmd == CMD_MIC || test_cmd == CMD_SPK) &&
                            test_len <= MAX_REASONABLE_PAYLOAD)
                        {
                            // Нашли потенциально валидный заголовок
                            std::cout << "[tcp] Found potential valid CMD packet at offset " << offset 
                                      << ", treating " << offset << " bytes before as raw data\n";
                            
                            // Обрабатываем данные до этого смещения как raw
                            if (rx_callback && offset > 0)
                            {
                                rx_callback(CMD_RESERVED, rx_buffer.data(), static_cast<u32>(offset));
                            }
                            
                            rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + offset);
                            skip_count = 0;
                            found_valid = true;
                            break;
                        }
                    }
                    
                    if (!found_valid)
                    {
                        // Не нашли валидный пакет, обрабатываем весь буфер как raw
                        std::cout << "[tcp] Data doesn't match CMD format, treating entire buffer as raw data\n";
                        std::cout << "[tcp] Raw data size: " << rx_buffer.size() << " bytes\n";
                        
                        if (rx_callback && !rx_buffer.empty())
                        {
                            rx_callback(CMD_RESERVED, rx_buffer.data(), static_cast<u32>(rx_buffer.size()));
                        }
                        
                        rx_buffer.clear();
                        skip_count = 0;
                        break;
                    }
                }
                else
                {
                    std::cerr << "  Skipping byte and retrying... (skip_count=" << skip_count << ")\n";
                    rx_buffer.erase(rx_buffer.begin());
                    continue;
                }
            }
        }
    }
};

#endif // TCP_HPP

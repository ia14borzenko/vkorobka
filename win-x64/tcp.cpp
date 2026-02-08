#include "tcp.hpp"
#include "message_router.hpp"
#include <iomanip>

// Forward declaration - определен в main.cpp
extern message_router_t* g_message_router;

tcp_t::tcp_t(std::atomic<bool>* g_running_p)
    : g_running(g_running_p)
    , listen_sock(INVALID_SOCKET)
    , client_sock(INVALID_SOCKET)
    , tcp_connected(false)
    , current_state(tcp_state_t::init)
    , rx_callback(nullptr)
{
}

tcp_t::~tcp_t()
{
    stop();
}

bool tcp_t::start()
{
    if (tcp_init() != 0)
    {
        return false;
    }

    worker = std::thread(&tcp_t::run, this);
    return true;
}

void tcp_t::stop()
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

SOCKET tcp_t::get_client_socket() const
{
    return client_sock;
}

bool tcp_t::is_connected() const
{
    return tcp_connected.load();
}

tcp_state_t tcp_t::get_state() const
{
    return current_state.load();
}

std::string tcp_t::get_state_string() const
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

void tcp_t::set_packet_callback(packet_callback_t callback)
{
    rx_callback = callback;
}

bool tcp_t::send(const std::string& data)
{
    return send(data.c_str(), static_cast<int>(data.size()));
}

bool tcp_t::send(const char* data, int len)
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

bool tcp_t::send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len)
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

bool tcp_t::send_packet(cmdcode_t cmd_code, const std::string& payload)
{
    return send_packet(cmd_code, payload.c_str(), static_cast<u32>(payload.size()));
}

bool tcp_t::send_message(const u8* buffer, u32 buffer_len)
{
    if (buffer == nullptr || buffer_len == 0)
    {
        return false;
    }
    
    return send(reinterpret_cast<const char*>(buffer), static_cast<int>(buffer_len));
}

int tcp_t::tcp_init()
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

bool tcp_t::start_listening()
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

bool tcp_t::accept_connection()
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

void tcp_t::read_initial_data()
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

void tcp_t::read_and_process_data()
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

void tcp_t::handle_connection_error()
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

bool tcp_t::connection_alive() const
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

void tcp_t::close_all()
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

void tcp_t::run()
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

void tcp_t::process_rx_buffer()
{
    // Обрабатываем только новый протокол message_protocol
    while (rx_buffer.size() >= MSG_HEADER_LEN && g_message_router)
    {
        // Пытаемся распарсить новый протокол
        msg_header_t header;
        const u8* payload = nullptr;
        u32 payload_len = 0;
        
        int unpack_result = msg_unpack(reinterpret_cast<const u8*>(rx_buffer.data()), 
                                       static_cast<u32>(rx_buffer.size()), 
                                       &header, &payload, &payload_len);
        
        if (unpack_result)
        {
            u32 expected_total = MSG_HEADER_LEN + payload_len;
            
            if (rx_buffer.size() >= expected_total)
            {
                // Передаем в message_router для обработки
                if (g_message_router->route_from_buffer(reinterpret_cast<const u8*>(rx_buffer.data()), 
                                                         expected_total))
                {
                    // Успешно обработано, удаляем из буфера
                    rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + expected_total);
                    // Продолжаем обработку следующего сообщения
                    continue;
                }
                else
                {
                    std::cerr << "[tcp] ERROR: message_router->route_from_buffer returned false\n";
                    // Не удалось обработать, выходим
                    break;
                }
            }
            else
            {
                // Недостаточно данных для полного пакета, ждем еще
                return;
            }
        }
        else
        {
            // Не удалось распарсить как новый протокол
            // Проверяем, валиден ли заголовок (чтобы понять, недостаточно данных или невалидный заголовок)
            if (rx_buffer.size() >= MSG_HEADER_LEN)
            {
                // Читаем заголовок вручную для проверки
                u32 payload_len_from_header = (u32)(rx_buffer[7] | (rx_buffer[8] << 8) | 
                                                    (rx_buffer[9] << 16) | (rx_buffer[10] << 24));
                u32 expected_total = MSG_HEADER_LEN + payload_len_from_header;
                
                // Проверяем валидность заголовка
                bool header_looks_valid = (header.msg_type >= MSG_TYPE_COMMAND && header.msg_type <= MSG_TYPE_ERROR) &&
                                         (header.source_id >= MSG_SRC_WIN && header.source_id <= MSG_SRC_EXTERNAL) &&
                                         (header.destination_id >= MSG_DST_WIN && header.destination_id <= MSG_DST_BROADCAST) &&
                                         (payload_len_from_header <= MSG_MAX_PAYLOAD_SIZE);
                
                if (header_looks_valid && rx_buffer.size() < expected_total)
                {
                    // Заголовок выглядит валидным, но данных недостаточно - ждем
                    return;
                }
                else if (header_looks_valid && rx_buffer.size() >= expected_total)
                {
                    // Заголовок валиден и данных достаточно, но unpack не прошел - странно
                    // Попробуем еще раз распарсить
                    continue;
                }
                else
                {
                    // Заголовок невалиден - сбрасываем буфер
                    // Правило: если не получилось обработать заголовок - он сбрасывается без уведомления отправителю
                    std::cerr << "[tcp] Invalid header, clearing buffer (" << rx_buffer.size() << " bytes)\n";
                    rx_buffer.clear();
                    return;
                }
            }
            else
            {
                // Недостаточно данных даже для заголовка, ждем
                return;
            }
        }
    }
}

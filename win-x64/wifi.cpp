#include "wifi.hpp"

bool wifi_t::start(void)
{
    if (wifi_init() != 0)
    {
        return false;
    }

    worker = std::thread(&wifi_t::run, this);
    return true;
}

void wifi_t::stop(void)
{
    g_running->store(false);

    if (worker.joinable())
    {
        worker.join();
    }

    close_all();
}

SOCKET wifi_t::get_client_socket(void) const
{
    return client_sock;
}

bool wifi_t::is_connected(void) const
{
    return g_wifi_up.load();
}

wifi_state_t wifi_t::get_state(void) const
{
    return current_state.load();
}

std::string wifi_t::get_state_string(void) const
{
    switch (current_state.load())
    {
    case wifi_state_t::init:
        return "INIT";
    case wifi_state_t::listening:
        return "LISTENING";
    case wifi_state_t::connected:
        return "CONNECTED";
    case wifi_state_t::lost:
        return "LOST";
    case wifi_state_t::retry:
        return "RETRY";
    default:
        return "UNKNOWN";
    }
}

void wifi_t::force_reconnect(void)
{
    std::cout << "[wifi] Force reconnect requested\n";
    // Закрываем все соединения
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
    g_wifi_up.store(false);
    // Устанавливаем состояние для немедленного переподключения
    current_state.store(wifi_state_t::lost);
}

int wifi_t::wifi_init(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "[wifi] WSAStartup failed: " << WSAGetLastError() << "\n";
        return -1;
    }

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET)
    {
        std::cerr << "[wifi] socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return -1;
    }

    //// �������� ����� ��� ������� � ������� ���������� ����� Ctrl+C
    // Включаем SO_REUSEADDR для возможности переиспользования порта
    char opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    return 0;
}

bool wifi_t::start_listening(void)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "[wifi] bind failed: " << WSAGetLastError() << "\n";
        return false;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "[wifi] listen failed: " << WSAGetLastError() << "\n";
        return false;
    }

    std::cout << "[wifi] Listening on port " << listen_port << "...\n";
    return true;
}

bool wifi_t::accept_connection(void)
{
    sockaddr_in client_addr{};
    int len = sizeof(client_addr);

    client_sock = accept(listen_sock, (sockaddr*)&client_addr, &len);
    if (client_sock == INVALID_SOCKET)
    {
        std::cerr << "[wifi] accept failed: " << WSAGetLastError() << "\n";
        return false;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

    std::cout << "[wifi] Connected: " << ip << ":" << ntohs(client_addr.sin_port) << "\n";
    return true;
}

bool wifi_t::connection_alive(void) const
{
    if (client_sock == INVALID_SOCKET) return false;

    char tmp;
    int ret = recv(client_sock, &tmp, 1, MSG_PEEK);

    if (ret == 0) return false;
    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        return (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
    }

    return true;
}

void wifi_t::close_all(void)
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

    g_wifi_up.store(false);
}

void wifi_t::run(void)
{
    wifi_state_t state = wifi_state_t::init;
    current_state.store(state);

    while (g_running->load())
    {
        // Обновляем атомарное состояние для мониторинга
        current_state.store(state);
        
        switch (state)
        {
        case wifi_state_t::init:
        {
            // Закрываем все сокеты перед инициализацией
            close_all();
            
            if (wifi_init() == 0)
            {
                state = wifi_state_t::listening;
            }
            else
            {
                state = wifi_state_t::retry;
            }
            break;
        }

        case wifi_state_t::listening:
        {
            if (start_listening())
            {
                state = wifi_state_t::connected;
            }
            else
            {
                // Если не удалось начать слушать, закрываем сокет и переходим в retry
                if (listen_sock != INVALID_SOCKET)
                {
                    closesocket(listen_sock);
                    listen_sock = INVALID_SOCKET;
                }
                state = wifi_state_t::retry;
            }
            break;
        }

        case wifi_state_t::connected:
        {
            if (accept_connection())
            {
                g_wifi_up.store(true);
                while (g_running->load() && connection_alive())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                }
                g_wifi_up.store(false);
                state = wifi_state_t::lost;
            }
            else
            {
                state = wifi_state_t::retry;
            }
            break;
        }

        case wifi_state_t::lost:
        {
            close_all();
            std::cout << "[wifi] Connection lost, will retry in 3 seconds...\n";
            state = wifi_state_t::retry;
            break;
        }

        case wifi_state_t::retry:
        {
            std::cout << "[wifi] Retrying connection...\n";
            std::this_thread::sleep_for(std::chrono::seconds(3));
            state = wifi_state_t::init;
            break;
        }
        }
    }

    close_all();
    current_state.store(wifi_state_t::init);
    std::cout << "[wifi] Server thread stopped\n";
}
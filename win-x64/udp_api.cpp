#include "udp_api.hpp"
#include <iostream>
#include <sstream>

// mstcpip.h не везде подключается до SIO_UDP_CONNRESET; значение из Windows SDK (WSAIoctl).
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET 0x9800000C
#endif

udp_api_t::udp_api_t(std::atomic<bool>* g_running_p, int port)
    : g_running(g_running_p)
    , sock(INVALID_SOCKET)
    , listen_port(port)
    , json_callback(nullptr)
{
}

udp_api_t::~udp_api_t()
{
    stop();
}

bool udp_api_t::start()
{
    if (udp_init() != 0)
    {
        return false;
    }
    
    worker = std::thread(&udp_api_t::run, this);
    return true;
}

void udp_api_t::stop()
{
    if (g_running)
    {
        g_running->store(false);
    }
    
    if (worker.joinable())
    {
        worker.join();
    }
    
    close_socket();
}

int udp_api_t::udp_init()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "[udp] WSAStartup failed: " << WSAGetLastError() << "\n";
        return -1;
    }
    
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        std::cerr << "[udp] socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return -1;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "[udp] bind failed: " << WSAGetLastError() << "\n";
        closesocket(sock);
        sock = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }
    
    // Неблокирующий режим
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    // После sendto на «мёртвый» UDP-порт Windows шлёт ICMP port unreachable; без этого
    // последующие recvfrom часто дают WSAECONNRESET (10054) и засыпают приём.
    BOOL udp_no_connreset = FALSE;
    DWORD ioctl_bytes = 0;
    if (WSAIoctl(sock, SIO_UDP_CONNRESET, &udp_no_connreset, sizeof(udp_no_connreset),
                 nullptr, 0, &ioctl_bytes, nullptr, nullptr) == SOCKET_ERROR)
    {
        std::cerr << "[udp] WSAIoctl(SIO_UDP_CONNRESET) failed: " << WSAGetLastError() << "\n";
    }
    
    std::cout << "[udp] UDP server started on port " << listen_port << "\n";
    return 0;
}

void udp_api_t::close_socket()
{
    if (sock != INVALID_SOCKET)
    {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
}

void udp_api_t::run()
{
    char buffer[65507]; // Максимальный размер UDP пакета
    sockaddr_in from_addr{};
    
    while (g_running && g_running->load())
    {
        // Обязательно на каждой итерации: иначе recvfrom на Windows даёт нестабильное поведение.
        int from_len = sizeof(from_addr);
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, 
                          (sockaddr*)&from_addr, &from_len);
        
        if (len > 0)
        {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from_addr.sin_addr, ip, sizeof(ip));
            int port = ntohs(from_addr.sin_port);
            
            // Регистрируем клиента
            register_client(ip, port);
            
            // Создаем строку из полученных данных
            std::string json_str(buffer, len);
            
            std::cout << "[udp] Received " << len << " bytes from " << ip << ":" << port << "\n";
            
            // Вызываем callback если установлен
            if (json_callback)
            {
                json_callback(json_str, ip, port);
            }
        }
        else if (len == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
            {
                // норма для неблокирующего сокета
            }
            else if (err == WSAECONNRESET)
            {
                // Часто следует за sendto на закрытый порт при рассылке всем клиентам;
                // не спим — иначе приём UDP фактически останавливается на секунды.
            }
            else
            {
                std::cerr << "[udp] recvfrom error: " << err << "\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    close_socket();
    std::cout << "[udp] UDP server stopped\n";
}

bool udp_api_t::send_json_to(const std::string& ip, int port, const std::string& json_str, int* out_wsa_error)
{
    if (sock == INVALID_SOCKET)
    {
        return false;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "[udp] Invalid IP address: " << ip << "\n";
        return false;
    }
    
    int sent = sendto(sock, json_str.c_str(), static_cast<int>(json_str.size()), 0, 
                     (sockaddr*)&addr, sizeof(addr));
    
    if (sent == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        if (out_wsa_error)
        {
            *out_wsa_error = err;
        }
        std::cerr << "[udp] sendto failed: " << err << "\n";
        return false;
    }
    
    return sent == static_cast<int>(json_str.size());
}

bool udp_api_t::send_json_to_all(const std::string& json_str)
{
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    bool all_sent = true;
    for (auto it = clients_.begin(); it != clients_.end(); )
    {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &it->second.sin_addr, ip, sizeof(ip));
        int port = ntohs(it->second.sin_port);
        
        int wsa = 0;
        if (!send_json_to(ip, port, json_str, &wsa))
        {
            all_sent = false;
            // Не удаляем живого клиента из‑за переполнения исходящего буфера (неблокирующий UDP).
            if (wsa == WSAEWOULDBLOCK)
            {
                ++it;
                continue;
            }
            std::cerr << "[udp] send_json_to failed (wsa=" << wsa << "), dropping client " << it->first << "\n";
            it = clients_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    
    return all_sent;
}

void udp_api_t::register_client(const std::string& ip, int port)
{
    std::lock_guard<std::mutex> lock(clients_mutex_);
    
    std::string key = get_client_key(ip, port);
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    
    clients_[key] = addr;
    
    std::cout << "[udp] Registered client: " << ip << ":" << port << "\n";
}

void udp_api_t::set_json_callback(udp_json_callback_t callback)
{
    json_callback = callback;
}

std::string udp_api_t::get_client_key(const std::string& ip, int port)
{
    std::ostringstream oss;
    oss << ip << ":" << port;
    return oss.str();
}

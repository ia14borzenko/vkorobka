#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

void rx_thread(SOCKET client_sock) {
    char buf[256];
    while (true) {
        int len = recv(client_sock, buf, sizeof(buf) - 1, 0);
        if (len <= 0) {
            std::cout << "Connection closed or error." << std::endl;
            break;
        }
        buf[len] = 0;
        std::cout << "ESP: " << buf;
    }
}

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cout << "WSAStartup failed." << std::endl;
        return 1;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cout << "Socket creation failed." << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = INADDR_ANY;  // Слушать на всех интерфейсах

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cout << "Bind failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::cout << "Listen failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Server listening on port 1234. Waiting for ESP32..." << std::endl;

    sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    SOCKET client_sock = accept(listen_sock, (sockaddr*)&client_addr, &client_addr_len);
    if (client_sock == INVALID_SOCKET) {
        std::cout << "Accept failed: " << WSAGetLastError() << std::endl;
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    std::cout << "Connected to ESP32 at " << client_ip << std::endl;

    // Закрываем listen_sock, так как чат один-на-один
    closesocket(listen_sock);

    std::thread rx(rx_thread, client_sock);

    std::string line;
    while (std::getline(std::cin, line)) {
        line += "\n";
        if (send(client_sock, line.c_str(), (int)line.size(), 0) == SOCKET_ERROR) {
            std::cout << "Send failed: " << WSAGetLastError() << std::endl;
            break;
        }
    }

    rx.join();
    closesocket(client_sock);
    WSACleanup();
    return 0;
}
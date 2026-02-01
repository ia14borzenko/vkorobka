#include "tcp.hpp"
#include "esp_log.h"
#include "errno.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>

static const char* TAG = "tcp";

tcp_t::tcp_t(const char* server_ip, int server_port, wifi_t* wifi)
    : server_ip(server_ip)
    , server_port(server_port)
    , wifi(wifi)
    , sock(-1)
    , state(TCP_STATE_INIT)
    , connected(false)
    , rx_callback(nullptr)
    , task_handle(nullptr)
{
}

tcp_t::~tcp_t()
{
    stop();
}

bool tcp_t::start(void)
{
    if (task_handle != nullptr)
    {
        ESP_LOGW(TAG, "TCP client task already started");
        return true;
    }

    BaseType_t ret = xTaskCreate(tcp_client_task, "tcp_client", 8192, this, 5, &task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create TCP client task");
        return false;
    }

    ESP_LOGI(TAG, "TCP client task created");
    return true;
}

void tcp_t::stop(void)
{
    if (task_handle != nullptr)
    {
        vTaskDelete(task_handle);
        task_handle = nullptr;
    }

    if (sock >= 0)
    {
        close(sock);
        sock = -1;
    }

    connected = false;
    rx_buffer.clear();
}

bool tcp_t::is_connected(void) const
{
    return connected && (sock >= 0);
}

void tcp_t::set_packet_callback(packet_callback_t callback)
{
    rx_callback = callback;
}

bool tcp_t::send(const char* data, int len)
{
    if (!is_connected() || sock < 0)
    {
        ESP_LOGE(TAG, "Cannot send: not connected");
        return false;
    }
    
    int sent = ::send(sock, data, len, 0);
    if (sent < 0)
    {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        return false;
    }
    
    return sent == len;
}

bool tcp_t::send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len)
{
    if (!is_connected() || sock < 0)
    {
        ESP_LOGE(TAG, "Cannot send packet: not connected");
        return false;
    }
    
    // Выделяем буфер для пакета
    std::vector<char> packet(CMD_HEADER_LEN + payload_len);
    
    // Упаковываем пакет
    u32 packet_size = pack_packet(cmd_code, payload, payload_len, packet.data());
    if (packet_size == 0)
    {
        ESP_LOGE(TAG, "Failed to pack packet");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending packet:");
    ESP_LOGI(TAG, "  CMD Code: 0x%04x", cmd_code);
    ESP_LOGI(TAG, "  Payload length: %u bytes", payload_len);
    ESP_LOGI(TAG, "  Total packet size: %u bytes", packet_size);
    
    // Отправляем все данные (может потребоваться несколько вызовов send)
    const char* ptr = packet.data();
    int remaining = static_cast<int>(packet_size);
    int total_sent = 0;
    
    while (remaining > 0)
    {
        int sent = ::send(sock, ptr, remaining, 0);
        if (sent < 0)
        {
            ESP_LOGE(TAG, "Send packet failed: errno %d", errno);
            return false;
        }
        if (sent == 0)
        {
            ESP_LOGE(TAG, "Connection closed during send");
            return false;
        }
        
        total_sent += sent;
        ptr += sent;
        remaining -= sent;
    }
    
    ESP_LOGI(TAG, "Packet sent successfully: %d bytes", total_sent);
    return true;
}

bool tcp_t::send_packet_str(cmdcode_t cmd_code, const char* str)
{
    return send_packet(cmd_code, str, static_cast<u32>(strlen(str)));
}

bool tcp_t::connection_alive(int sock) const
{
    if (sock < 0) return false;
    
    // Простая проверка через getsockopt(SO_ERROR)
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0)
    {
        if (error != 0)
        {
            // Есть ошибка на сокете
            return false;
        }
    }
    else
    {
        // Ошибка getsockopt - соединение неактивно
        return false;
    }
    
    return true;
}

void tcp_t::process_rx_buffer(void)
{
    u32 skip_count = 0;  // Счетчик пропущенных байт
    const u32 MAX_REASONABLE_PAYLOAD = 65535;  // Максимальная разумная длина payload
    const u32 MAX_SKIP_BEFORE_CLEAR = CMD_HEADER_LEN;  // После пропуска CMD_HEADER_LEN байт очищаем буфер
    
    while (rx_buffer.size() >= CMD_HEADER_LEN)
    {
        // Проверяем, есть ли полный заголовок
        const char* msg = rx_buffer.data();
        u32 msg_len = static_cast<u32>(rx_buffer.size());
        
        // Читаем заголовок для проверки длины (читаем байты напрямую для правильного порядка байт)
        // CMD Code: 2 байта (little-endian)
        // Payload length: 4 байта (little-endian)
        cmdcode_t cmd_code_raw = (cmdcode_t)(msg[0] | (msg[1] << 8));
        u32 payload_len_raw = (u32)(msg[2] | (msg[3] << 8) | (msg[4] << 16) | (msg[5] << 24));
        
        // Проверка на разумность payload_len перед дальнейшей обработкой
        if (payload_len_raw > MAX_REASONABLE_PAYLOAD)
        {
            // payload_len явно невалидный
            skip_count++;
            ESP_LOGW(TAG, "Invalid payload length: %u (too large), skipping byte (skip_count=%u)", 
                     payload_len_raw, skip_count);
            
            // Если пропустили слишком много байт, очищаем буфер полностью
            if (skip_count >= MAX_SKIP_BEFORE_CLEAR)
            {
                ESP_LOGW(TAG, "Too many invalid bytes, clearing RX buffer completely");
                rx_buffer.clear();
                skip_count = 0;
                break;
            }
            
            rx_buffer.erase(rx_buffer.begin());
            continue;
        }
        
        u32 expected_total = CMD_HEADER_LEN + payload_len_raw;
        
        ESP_LOGI(TAG, "Checking packet header:");
        ESP_LOGI(TAG, "  CMD Code: 0x%04x", cmd_code_raw);
        ESP_LOGI(TAG, "  Payload length (from header): %u bytes", payload_len_raw);
        ESP_LOGI(TAG, "  Expected total packet size: %u bytes", expected_total);
        ESP_LOGI(TAG, "  Current buffer size: %u bytes", msg_len);
        

        // Пытаемся распарсить пакет
        const char* payload_begin = nullptr;
        u32 payload_len = 0;
        cmdcode_t cmd_code = is_pack(msg, expected_total, &payload_begin, &payload_len);
        
        if (cmd_code != CMD_RESERVED) 
        {
            // Сбрасываем счетчик при успешном парсинге
            skip_count = 0;
            
            ESP_LOGI(TAG, "CMD format check PASSED");
            ESP_LOGI(TAG, "  Valid CMD Code: 0x%04x", cmd_code);
            ESP_LOGI(TAG, "  Payload length: %u bytes", payload_len);

            // Проверяем, есть ли полный пакет
            if (msg_len < expected_total)
            {
                // Недостаточно данных для полного пакета, ждем еще
                ESP_LOGI(TAG, "Incomplete packet, waiting for more data (need %u more bytes)", 
                        expected_total - msg_len);
                break;  // Выходим из цикла, чтобы дождаться больше данных
            }
            
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
            
            // Удаляем весь обработанный пакет из буфера
            rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + expected_total);
            continue;  // Продолжаем обработку следующего пакета
        }
        else
        {
            // Неверный формат пакета, пропускаем один байт и пытаемся снова
            skip_count++;
            ESP_LOGW(TAG, "Invalid packet format - CMD format check FAILED");
            ESP_LOGW(TAG, "  Header CMD Code: 0x%04x", cmd_code_raw);
            ESP_LOGW(TAG, "  Header Payload Length: %u", payload_len_raw);
            ESP_LOGW(TAG, "  Skipping byte and retrying... (skip_count=%u)", skip_count);
            
            // Если пропустили слишком много байт, очищаем буфер полностью
            if (skip_count >= MAX_SKIP_BEFORE_CLEAR)
            {
                ESP_LOGW(TAG, "Too many invalid bytes, clearing RX buffer completely");
                rx_buffer.clear();
                skip_count = 0;
                break;
            }
            
            rx_buffer.erase(rx_buffer.begin());
            continue;  // Продолжаем с пропущенным байтом
        }
    }
}

void tcp_t::tcp_client_task(void* pvParameters)
{
    tcp_t* tcp = static_cast<tcp_t*>(pvParameters);
    
    ESP_LOGI(TAG, "TCP client task started");
    
    tcp->state = TCP_STATE_INIT;
    char rx_raw[tcp_t::TCP_RX_BUFFER_SIZE];
    
    while (1)
    {
        // Проверяем, что Wi-Fi подключен
        if (tcp->wifi == nullptr)
        {
            ESP_LOGE(TAG, "WiFi pointer is null!");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        if (!tcp->wifi->is_connected())
        {
            ESP_LOGW(TAG, "Wi-Fi not connected, waiting... (state: %s)", 
                     tcp->state == TCP_STATE_INIT ? "INIT" :
                     tcp->state == TCP_STATE_CONNECTING ? "CONNECTING" :
                     tcp->state == TCP_STATE_CONNECTED ? "CONNECTED" :
                     tcp->state == TCP_STATE_LOST ? "LOST" : "RETRY");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        switch (tcp->state)
        {
            case TCP_STATE_INIT:
            {
                ESP_LOGI(TAG, "TCP state: INIT");
                tcp->state = TCP_STATE_CONNECTING;
                break;
            }
            
            case TCP_STATE_CONNECTING:
            {
                ESP_LOGI(TAG, "Attempting to connect to server...");
                if (connect_to_server(tcp))
                {
                    ESP_LOGI(TAG, "Connection established, reading initial data");
                    read_initial_data(tcp);
                    tcp->state = TCP_STATE_CONNECTED;
                    ESP_LOGI(TAG, "TCP state: CONNECTED");
                }
                else
                {
                    ESP_LOGW(TAG, "Connection failed, will retry");
                    tcp->state = TCP_STATE_RETRY;
                }
                break;
            }
            
            case TCP_STATE_CONNECTED:
            {
                // Event-driven чтение с select()
                fd_set readfds, errorfds;
                FD_ZERO(&readfds);
                FD_ZERO(&errorfds);
                FD_SET(tcp->sock, &readfds);
                FD_SET(tcp->sock, &errorfds);
                
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000; // 100ms
                
                int ret = select(tcp->sock + 1, &readfds, nullptr, &errorfds, &timeout);
                
                if (ret > 0)
                {
                    if (FD_ISSET(tcp->sock, &errorfds))
                    {
                        ESP_LOGW(TAG, "Connection error detected by select");
                        tcp->state = TCP_STATE_LOST;
                        break;
                    }
                    
                    if (FD_ISSET(tcp->sock, &readfds))
                    {
                        ESP_LOGI(TAG, "Data available on socket, reading...");
                        read_and_process_data(tcp, rx_raw);
                    }
                }
                else if (ret < 0)
                {
                    // Ошибка select
                    ESP_LOGE(TAG, "select error: errno %d", errno);
                    tcp->state = TCP_STATE_LOST;
                }
                // ret == 0 означает timeout - это нормально, продолжаем
                
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
            
            case TCP_STATE_LOST:
            {
                ESP_LOGI(TAG, "TCP state: LOST");
                tcp->connected = false;
                
                if (tcp->sock >= 0)
                {
                    close(tcp->sock);
                    tcp->sock = -1;
                }
                
                tcp->rx_buffer.clear();
                tcp->state = TCP_STATE_RETRY;
                break;
            }
            
            case TCP_STATE_RETRY:
            {
                ESP_LOGI(TAG, "TCP state: RETRY (waiting %d ms)", tcp_t::TCP_RECONNECT_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(tcp_t::TCP_RECONNECT_DELAY_MS));
                tcp->state = TCP_STATE_INIT;
                break;
            }
        }
    }
    
    // Очистка при выходе
    if (tcp->sock >= 0)
    {
        close(tcp->sock);
        tcp->sock = -1;
    }
    tcp->connected = false;
    vTaskDelete(NULL);
}

bool tcp_t::connect_to_server(tcp_t* tcp)
{
    // Проверка валидности указателя
    if (tcp == nullptr || tcp->server_ip == nullptr)
    {
        ESP_LOGE(TAG, "Invalid tcp pointer or server_ip in connect_to_server");
        return false;
    }
    
    ESP_LOGI(TAG, "TCP state: CONNECTING to %s:%d", tcp->server_ip, tcp->server_port);
    
    // Создаем сокет
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return false;
    }
    
    // Устанавливаем опции сокета
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Устанавливаем таймауты
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // Настраиваем адрес сервера
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(tcp->server_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(tcp->server_port);
    
    // Подключаемся
    int err = connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (err != 0)
    {
        ESP_LOGE(TAG, "Socket connect failed: errno %d", errno);
        close(sock);
        return false;
    }
    
    // Включаем TCP keepalive для автоматического обнаружения разрыва
    int keepalive = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0)
    {
        ESP_LOGE(TAG, "Failed to set SO_KEEPALIVE: errno %d", errno);
    }
    
    // Настраиваем параметры TCP keepalive для быстрого обнаружения разрыва (~5 секунд)
    // В lwip параметры могут быть доступны через IPPROTO_TCP
    // TCP_KEEPIDLE: время до первого keepalive probe (2 секунды)
    // TCP_KEEPINTVL: интервал между probes (1 секунда)
    // TCP_KEEPCNT: количество неудачных probes до разрыва (3)
    // Итого: 2 + 1*3 = 5 секунд до обнаружения разрыва
    
    int keepidle = 2;      // 2 секунды до первого probe
    int keepintvl = 1;     // 1 секунда между probes
    int keepcount = 3;     // 3 неудачных probe = разрыв
    
    // Пытаемся установить параметры keepalive
    // В lwip эти параметры могут быть доступны, но не всегда
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0)
    {
        ESP_LOGW(TAG, "TCP_KEEPIDLE not supported (errno %d), using defaults", errno);
    }
    
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0)
    {
        ESP_LOGW(TAG, "TCP_KEEPINTVL not supported (errno %d), using defaults", errno);
    }
    
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount)) < 0)
    {
        ESP_LOGW(TAG, "TCP_KEEPCNT not supported (errno %d), using defaults", errno);
    }
    
    ESP_LOGI(TAG, "TCP keepalive configured: idle=2s, interval=1s, count=3 (~5s detection)");
    
    // В ESP-IDF не нужно устанавливать неблокирующий режим через fcntl
    // Используем MSG_DONTWAIT при вызовах recv() вместо этого
    
    // Сохраняем сокет и состояние перед вызовом read_initial_data
    tcp->sock = sock;
    tcp->connected = true;
    tcp->rx_buffer.clear();
    
    ESP_LOGI(TAG, "Connected successfully to %s:%d (socket: %d)", tcp->server_ip, tcp->server_port, sock);
    return true;
}

void tcp_t::read_initial_data(tcp_t* tcp)
{
    // Проверка валидности указателя и сокета
    if (tcp == nullptr || tcp->sock < 0)
    {
        ESP_LOGE(TAG, "Invalid tcp pointer or socket in read_initial_data");
        return;
    }
    
    ESP_LOGI(TAG, "Reading initial data after connect (socket: %d)", tcp->sock);
    
    // Немедленное чтение всех доступных данных после connect
    // Используем меньший буфер для начального чтения, чтобы избежать проблем со стеком
    const int INITIAL_READ_SIZE = 1024;
    char initial_rx[INITIAL_READ_SIZE];
    
    // Читаем в цикле пока есть данные (неблокирующий recv)
    while (true)
    {
        int initial_len = recv(tcp->sock, initial_rx, INITIAL_READ_SIZE, MSG_DONTWAIT);
        
        if (initial_len > 0)
        {
            ESP_LOGI(TAG, "Received %d bytes (initial data after connect)", initial_len);
            tcp->rx_buffer.insert(tcp->rx_buffer.end(), initial_rx, initial_rx + initial_len);
        }
        else if (initial_len == 0)
        {
            // Соединение закрыто сразу после connect
            ESP_LOGW(TAG, "Connection closed immediately after connect");
            close(tcp->sock);
            tcp->sock = -1;
            tcp->connected = false;
            return;
        }
        else
        {
            // EAGAIN/EWOULDBLOCK - данных больше нет
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK)
            {
                ESP_LOGI(TAG, "No initial data available (EAGAIN/EWOULDBLOCK)");
                break;
            }
            else
            {
                ESP_LOGE(TAG, "Error reading initial data: errno %d", err);
                return;
            }
        }
    }
    
    // Обрабатываем начальные данные
    if (!tcp->rx_buffer.empty())
    {
        ESP_LOGI(TAG, "Processing %u bytes of initial data", (unsigned int)tcp->rx_buffer.size());
        tcp->process_rx_buffer();
    }
    else
    {
        ESP_LOGI(TAG, "No initial data to process");
    }
}

void tcp_t::read_and_process_data(tcp_t* tcp, char* rx_raw)
{
    // Проверка валидности указателя и сокета
    if (tcp == nullptr || tcp->sock < 0 || rx_raw == nullptr)
    {
        ESP_LOGE(TAG, "Invalid parameters in read_and_process_data");
        return;
    }
    
    // Чтение данных в цикле пока есть данные (неблокирующий recv)
    while (true)
    {
        int len = recv(tcp->sock, rx_raw, tcp_t::TCP_RX_BUFFER_SIZE, MSG_DONTWAIT);
        
        if (len > 0)
        {
            ESP_LOGI(TAG, "Received %d bytes from socket", len);
            tcp->rx_buffer.insert(tcp->rx_buffer.end(), rx_raw, rx_raw + len);
            ESP_LOGI(TAG, "RX buffer size: %u bytes", (unsigned int)tcp->rx_buffer.size());
            tcp->process_rx_buffer();
        }
        else if (len == 0)
        {
            // Соединение закрыто сервером
            ESP_LOGI(TAG, "Connection closed by server");
            tcp->state = TCP_STATE_LOST;
            return;
        }
        else
        {
            // EAGAIN/EWOULDBLOCK - данных больше нет
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK)
            {
                break;
            }
            else
            {
                ESP_LOGE(TAG, "recv error: errno %d", err);
                tcp->state = TCP_STATE_LOST;
                return;
            }
        }
    }
}

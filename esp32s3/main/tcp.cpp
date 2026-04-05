#include "tcp.hpp"
#include "esp_log.h"
#include "errno.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include "message_protocol.h"
#include "message_bridge.hpp"

static const char* TAG = "tcp";

// Forward declaration - определен в main.cpp
extern message_bridge_t* g_message_bridge;

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

    // Выше приоритета mic_stream/dyn_playback — быстрее drain TCP при потоке PCM
    BaseType_t ret = xTaskCreate(tcp_client_task, "tcp_client", 8192, this, 10, &task_handle);
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

tcp_state_t tcp_t::get_state(void) const
{
    return state;
}

const char* tcp_t::get_state_string(void) const
{
    switch (state)
    {
    case TCP_STATE_INIT:
        return "INIT";
    case TCP_STATE_CONNECTING:
        return "CONNECTING";
    case TCP_STATE_CONNECTED:
        return "CONNECTED";
    case TCP_STATE_LOST:
        return "LOST";
    case TCP_STATE_RETRY:
        return "RETRY";
    default:
        return "UNKNOWN";
    }
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
    
    // Старый CMD протокол больше не используется
    // send_packet оставлен для совместимости, но не должен использоваться
    ESP_LOGW(TAG, "send_packet called but old CMD protocol is no longer supported");
    return false;
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
    // Обрабатываем только новый протокол message_protocol
    while (rx_buffer.size() >= MSG_HEADER_LEN)
    {
        msg_header_t header;
        const u8* payload = nullptr;
        u32 payload_len = 0;
        
        // Пытаемся распарсить как новый протокол
        int unpack_result = msg_unpack(reinterpret_cast<const u8*>(rx_buffer.data()), 
                                      static_cast<u32>(rx_buffer.size()), 
                                      &header, &payload, &payload_len);
        
        ESP_LOGD(TAG, "[DEBUG] TCP unpack: result=%d, payload_len=%u, header.payload_len=%u",
                 unpack_result, payload_len, header.payload_len);
        
        if (unpack_result)
        {
            // Проверяем, есть ли полный пакет
            u32 expected_total = MSG_HEADER_LEN + payload_len;
            
            ESP_LOGD(TAG, "[DEBUG] TCP: expected_total=%u, rx_buffer.size()=%zu", expected_total,
                     rx_buffer.size());

            if (rx_buffer.size() >= MSG_HEADER_LEN)
            {
                char hex_buf[64];
                int hex_len = 0;
                for (int i = 0; i < MSG_HEADER_LEN && i < 12; i++)
                {
                    hex_len += sprintf(hex_buf + hex_len, "%02X ", rx_buffer[i]);
                }
                ESP_LOGD(TAG, "[DEBUG] TCP header bytes: %s", hex_buf);
                ESP_LOGD(TAG, "[DEBUG] TCP payload_len bytes [7-10]: %02X %02X %02X %02X", rx_buffer[7],
                         rx_buffer[8], rx_buffer[9], rx_buffer[10]);
            }
            
            if (rx_buffer.size() >= expected_total)
            {
                // Передаем в message_bridge
                if (g_message_bridge)
                {
                    g_message_bridge->process_buffer(
                        reinterpret_cast<const u8*>(rx_buffer.data()), 
                        static_cast<u32>(expected_total)
                    );
                }
                
                // Удаляем обработанный пакет из буфера
                rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + expected_total);
                continue;  // Продолжаем обработку следующего пакета
            }
            else
            {
                // Недостаточно данных для полного пакета, ждем еще
                break;  // Выходим из цикла, чтобы дождаться больше данных
            }
        }
        else
        {
            // Не удалось распарсить как новый протокол
            // Проверяем, может быть данных недостаточно для полного пакета
            if (rx_buffer.size() >= MSG_HEADER_LEN)
            {
                // Читаем заголовок вручную для проверки
                u8 msg_type = rx_buffer[0];
                u8 source_id = rx_buffer[1];
                u8 destination_id = rx_buffer[2];
                u32 payload_len_from_header = (u32)(rx_buffer[7] | (rx_buffer[8] << 8) | 
                                                    (rx_buffer[9] << 16) | (rx_buffer[10] << 24));
                u32 expected_total = MSG_HEADER_LEN + payload_len_from_header;
                
                // Проверяем валидность заголовка
                bool header_looks_valid = (msg_type >= MSG_TYPE_COMMAND && msg_type <= MSG_TYPE_ERROR) &&
                                         (source_id >= MSG_SRC_WIN && source_id <= MSG_SRC_EXTERNAL) &&
                                         (destination_id >= MSG_DST_WIN && destination_id <= MSG_DST_BROADCAST) &&
                                         (payload_len_from_header <= MSG_MAX_PAYLOAD_SIZE);
                
                if (header_looks_valid && rx_buffer.size() < expected_total)
                {
                    ESP_LOGD(TAG, "Valid header detected, waiting for more data (have %zu, need %u)",
                             rx_buffer.size(), expected_total);
                    break;
                }
                else if (header_looks_valid && rx_buffer.size() >= expected_total)
                {
                    ESP_LOGW(TAG,
                             "msg_unpack failed but manual header OK; skip 1 byte to resync (had %zu)",
                             rx_buffer.size());
                    rx_buffer.erase(rx_buffer.begin(), rx_buffer.begin() + 1);
                    continue;
                }
                else
                {
                    // Заголовок невалиден - сбрасываем буфер
                    // Правило: если не получилось обработать заголовок - он сбрасывается без уведомления отправителю
                    ESP_LOGW(TAG, "Invalid header, clearing buffer (%zu bytes)", rx_buffer.size());
                    rx_buffer.clear();
                    break;
                }
            }
            else
            {
                // Недостаточно данных даже для заголовка - ждем
                break;
            }
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
                        ESP_LOGD(TAG, "Data available on socket, reading...");
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
                
                vTaskDelay(pdMS_TO_TICKS(2));
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
            ESP_LOGD(TAG, "Received %d bytes from socket", len);
            tcp->rx_buffer.insert(tcp->rx_buffer.end(), rx_raw, rx_raw + len);
            ESP_LOGD(TAG, "RX buffer size: %u bytes", (unsigned int)tcp->rx_buffer.size());
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

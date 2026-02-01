#ifndef TCP_HPP
#define TCP_HPP

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <vector>
#include "netpack.h"
#include "wifi.hpp"

// State machine для TCP соединения
typedef enum {
    TCP_STATE_INIT,
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_LOST,
    TCP_STATE_RETRY
} tcp_state_t;

// Callback для обработки принятых пакетов
typedef void (*packet_callback_t)(cmdcode_t cmd_code, const char* payload, u32 payload_len);

class tcp_t
{
public:
    tcp_t(const char* server_ip, int server_port, wifi_t* wifi);
    ~tcp_t();

    // Запуск TCP клиента (создает задачу)
    bool start(void);
    
    // Остановка TCP клиента
    void stop(void);
    
    // Проверка, подключен ли TCP
    bool is_connected(void) const;
    
    // Установка callback для обработки принятых пакетов
    void set_packet_callback(packet_callback_t callback);
    
    // Отправка данных через TCP (raw, без упаковки)
    bool send(const char* data, int len);
    
    // Отправка пакета в формате CMD (с автоматической упаковкой)
    bool send_packet(cmdcode_t cmd_code, const void* payload, u32 payload_len);
    
    // Удобная перегрузка для отправки строки
    bool send_packet_str(cmdcode_t cmd_code, const char* str);

private:
    const char* server_ip;
    int server_port;
    wifi_t* wifi;
    
    int sock;
    tcp_state_t state;
    bool connected;
    packet_callback_t rx_callback;
    std::vector<char> rx_buffer;
    TaskHandle_t task_handle;
    
    static constexpr int TCP_RECONNECT_DELAY_MS = 3000;
    static constexpr int TCP_RX_BUFFER_SIZE = 4096;
    
    // Внутренние методы
    bool connection_alive(int sock) const;
    void process_rx_buffer(void);
    static void tcp_client_task(void* pvParameters);
    static bool connect_to_server(tcp_t* tcp);
    static void read_initial_data(tcp_t* tcp);
    static void read_and_process_data(tcp_t* tcp, char* rx_raw);
};

#endif // TCP_HPP

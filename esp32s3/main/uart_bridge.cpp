#include "uart_bridge.hpp"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "uart_bridge";

uart_bridge_t::uart_bridge_t(uart_port_t uart_num, int baud_rate)
    : uart_num_(uart_num)
    , baud_rate_(baud_rate)
    , initialized_(false)
    , rx_callback_(nullptr)
    , rx_task_handle_(nullptr)
    , tx_task_handle_(nullptr)
    , tx_queue_(nullptr)
{
}

uart_bridge_t::~uart_bridge_t()
{
    stop();
}

bool uart_bridge_t::init(int tx_pin, int rx_pin)
{
    if (initialized_)
    {
        ESP_LOGW(TAG, "UART already initialized");
        return true;
    }

    // Конфигурация UART
    uart_config_t uart_config = {
        .baud_rate = baud_rate_,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Устанавливаем конфигурацию
    esp_err_t ret = uart_driver_install(uart_num_, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return false;
    }

    ret = uart_param_config(uart_num_, &uart_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        uart_driver_delete(uart_num_);
        return false;
    }

    ret = uart_set_pin(uart_num_, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(uart_num_);
        return false;
    }

    // Создаем очередь для отправки
    tx_queue_ = xQueueCreate(UART_QUEUE_SIZE, sizeof(std::vector<u8>*));
    if (tx_queue_ == NULL)
    {
        ESP_LOGE(TAG, "Failed to create TX queue");
        uart_driver_delete(uart_num_);
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "UART initialized on port %d, baud rate %d", uart_num_, baud_rate_);
    return true;
}

bool uart_bridge_t::start()
{
    if (!initialized_)
    {
        ESP_LOGE(TAG, "UART not initialized");
        return false;
    }

    // Создаем задачу для чтения
    BaseType_t ret = xTaskCreate(uart_rx_task, "uart_rx", 4096, this, 5, &rx_task_handle_);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create RX task");
        return false;
    }

    // Создаем задачу для записи (увеличиваем стек из-за отладочного вывода)
    ret = xTaskCreate(uart_tx_task, "uart_tx", 4096, this, 5, &tx_task_handle_);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create TX task");
        vTaskDelete(rx_task_handle_);
        rx_task_handle_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "UART bridge tasks started");
    return true;
}

void uart_bridge_t::stop()
{
    if (rx_task_handle_ != nullptr)
    {
        vTaskDelete(rx_task_handle_);
        rx_task_handle_ = nullptr;
    }

    if (tx_task_handle_ != nullptr)
    {
        vTaskDelete(tx_task_handle_);
        tx_task_handle_ = nullptr;
    }

    if (tx_queue_ != nullptr)
    {
        vQueueDelete(tx_queue_);
        tx_queue_ = nullptr;
    }

    if (initialized_)
    {
        uart_driver_delete(uart_num_);
        initialized_ = false;
    }
}

bool uart_bridge_t::send_message(const msg_header_t* header, const u8* payload, u32 payload_len)
{
    if (!initialized_ || header == nullptr)
    {
        return false;
    }

    ESP_LOGI(TAG, "[DEBUG] send_message: dst=%d, payload_len=%u, header->payload_len=%u", 
             header->destination_id, payload_len, header->payload_len);

    // Упаковываем сообщение
    u32 total_size = MSG_HEADER_LEN + payload_len;
    std::vector<u8>* buffer = new std::vector<u8>(total_size);
    
    u32 packed_size = msg_pack(header, payload, payload_len, buffer->data());
    ESP_LOGI(TAG, "[DEBUG] msg_pack: packed_size=%u", packed_size);
    
    if (packed_size == 0)
    {
        ESP_LOGE(TAG, "[DEBUG] msg_pack failed!");
        delete buffer;
        return false;
    }

    buffer->resize(packed_size);
    
    // Показываем только критичные байты payload_len [7-10] (без больших буферов)
    ESP_LOGI(TAG, "[PACK] payload_len[7-10]=%02X %02X %02X %02X", 
             buffer->data()[7], buffer->data()[8], 
             buffer->data()[9], buffer->data()[10]);
    
    if (payload_len > 0 && payload_len <= 2)
    {
        ESP_LOGI(TAG, "[PACK] Payload[0-1]=%02X %02X", 
                 payload_len > 0 ? payload[0] : 0, 
                 payload_len > 1 ? payload[1] : 0);
    }

    // Отправляем в очередь
    if (xQueueSend(tx_queue_, &buffer, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to queue message for transmission");
        delete buffer;
        return false;
    }

    return true;
}

void uart_bridge_t::set_message_callback(uart_message_callback_t callback)
{
    rx_callback_ = callback;
}

void uart_bridge_t::uart_rx_task(void* pvParameters)
{
    uart_bridge_t* bridge = static_cast<uart_bridge_t*>(pvParameters);
    uint8_t* data = (uint8_t*)malloc(UART_RX_BUF_SIZE);
    
    if (data == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART RX task started");

    while (1)
    {
        int len = uart_read_bytes(bridge->uart_num_, data, UART_RX_BUF_SIZE, pdMS_TO_TICKS(100));
        
        if (len > 0)
        {
            // Добавляем данные в буфер
            bridge->rx_buffer_.insert(bridge->rx_buffer_.end(), data, data + len);
            
            // Обрабатываем буфер
            bridge->process_rx_buffer();
        }
    }

    free(data);
    vTaskDelete(NULL);
}

void uart_bridge_t::uart_tx_task(void* pvParameters)
{
    uart_bridge_t* bridge = static_cast<uart_bridge_t*>(pvParameters);
    std::vector<u8>* buffer = nullptr;

    ESP_LOGI(TAG, "UART TX task started");

    while (1)
    {
        if (xQueueReceive(bridge->tx_queue_, &buffer, portMAX_DELAY) == pdTRUE)
        {
            if (buffer != nullptr)
            {
                int len = uart_write_bytes(bridge->uart_num_, buffer->data(), buffer->size());
                if (len < 0)
                {
                    ESP_LOGE(TAG, "UART write failed");
                }
                else
                {
                    ESP_LOGI(TAG, "Sent %d bytes via UART", len);
                    // Отладочный вывод: показываем только критичные байты payload_len (без больших буферов)
                    if (len >= 11)
                    {
                        ESP_LOGI(TAG, "[TX] payload_len[7-10]=%02X %02X %02X %02X", 
                                 buffer->data()[7], buffer->data()[8], 
                                 buffer->data()[9], buffer->data()[10]);
                    }
                }
                
                delete buffer;
            }
        }
    }

    vTaskDelete(NULL);
}

void uart_bridge_t::process_rx_buffer()
{
    while (rx_buffer_.size() >= MSG_HEADER_LEN)
    {
        msg_header_t header;
        const u8* payload = nullptr;
        u32 payload_len = 0;

        if (!msg_unpack(rx_buffer_.data(), static_cast<u32>(rx_buffer_.size()), &header, &payload, &payload_len))
        {
            // Недостаточно данных или ошибка парсинга
            // Если это не начало валидного сообщения, пропускаем байт
            if (rx_buffer_.size() > MSG_HEADER_LEN * 2)
            {
                // Пропускаем первый байт и пытаемся снова
                rx_buffer_.erase(rx_buffer_.begin());
            }
            else
            {
                // Ждем больше данных
                break;
            }
        }
        else
        {
            // Успешно распарсили сообщение
            u32 total_size = MSG_HEADER_LEN + payload_len;
            rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + total_size);

            // Вызываем callback
            if (rx_callback_)
            {
                rx_callback_(&header, payload, payload_len);
            }
        }
    }
}

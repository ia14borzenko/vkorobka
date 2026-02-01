#ifndef UART_HANDLER_C_WRAPPER_H
#define UART_HANDLER_C_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "message_protocol.h"

// Непрозрачный указатель на C++ объект
typedef void* uart_handler_handle_t;

// Callback для обработки принятых сообщений (C-совместимый)
typedef void (*uart_message_handler_cb_t)(const msg_header_t* header, const u8* payload, u32 payload_len);

// C-интерфейс для работы с uart_handler_t

// Создать экземпляр uart_handler
uart_handler_handle_t uart_handler_create(void);

// Уничтожить экземпляр uart_handler
void uart_handler_destroy(uart_handler_handle_t handle);

// Инициализация UART
// uart_num: 1 для USART1, 2 для USART2
// baud_rate: скорость передачи (например, 115200)
// Возвращает: 1 при успехе, 0 при ошибке
int uart_handler_init(uart_handler_handle_t handle, int uart_num, int baud_rate);

// Запуск обработки
// Возвращает: 1 при успехе, 0 при ошибке
int uart_handler_start(uart_handler_handle_t handle);

// Остановка обработки
void uart_handler_stop(uart_handler_handle_t handle);

// Отправка сообщения
// Возвращает: 1 при успехе, 0 при ошибке
int uart_handler_send_message(uart_handler_handle_t handle, 
                               const msg_header_t* header, 
                               const u8* payload, 
                               u32 payload_len);

// Обработка входящих данных (вызывать в основном цикле или из прерывания)
void uart_handler_process_rx_data(uart_handler_handle_t handle);

// Регистрация обработчика по типу сообщения
void uart_handler_register_type_handler(uart_handler_handle_t handle,
                                        msg_type_t msg_type,
                                        uart_message_handler_cb_t handler);

// Регистрация обработчика по получателю
void uart_handler_register_dest_handler(uart_handler_handle_t handle,
                                         msg_destination_t destination,
                                         uart_message_handler_cb_t handler);

// Проверка, инициализирован ли UART
// Возвращает: 1 если инициализирован, 0 если нет
int uart_handler_is_initialized(uart_handler_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // UART_HANDLER_C_WRAPPER_H

#include "uart_handler_c_wrapper.h"
#include "uart_handler.hpp"

extern "C" {

uart_handler_handle_t uart_handler_create(void)
{
    return reinterpret_cast<uart_handler_handle_t>(new uart_handler_t());
}

void uart_handler_destroy(uart_handler_handle_t handle)
{
    if (handle != nullptr)
    {
        delete reinterpret_cast<uart_handler_t*>(handle);
    }
}

int uart_handler_init(uart_handler_handle_t handle, int uart_num, int baud_rate)
{
    if (handle == nullptr)
    {
        return 0;
    }
    
    uart_handler_t* handler = reinterpret_cast<uart_handler_t*>(handle);
    return handler->init(uart_num, baud_rate) ? 1 : 0;
}

int uart_handler_start(uart_handler_handle_t handle)
{
    if (handle == nullptr)
    {
        return 0;
    }
    
    uart_handler_t* handler = reinterpret_cast<uart_handler_t*>(handle);
    return handler->start() ? 1 : 0;
}

void uart_handler_stop(uart_handler_handle_t handle)
{
    if (handle == nullptr)
    {
        return;
    }
    
    uart_handler_t* handler = reinterpret_cast<uart_handler_t*>(handle);
    handler->stop();
}

int uart_handler_send_message(uart_handler_handle_t handle, 
                               const msg_header_t* header, 
                               const u8* payload, 
                               u32 payload_len)
{
    if (handle == nullptr || header == nullptr)
    {
        return 0;
    }
    
    uart_handler_t* handler = reinterpret_cast<uart_handler_t*>(handle);
    return handler->send_message(*header, payload, payload_len) ? 1 : 0;
}

void uart_handler_process_rx_data(uart_handler_handle_t handle)
{
    if (handle == nullptr)
    {
        return;
    }
    
    uart_handler_t* handler = reinterpret_cast<uart_handler_t*>(handle);
    handler->process_rx_data();
}

void uart_handler_register_type_handler(uart_handler_handle_t handle,
                                        msg_type_t msg_type,
                                        uart_message_handler_cb_t handler)
{
    if (handle == nullptr || handler == nullptr)
    {
        return;
    }
    
    uart_handler_t* uart_handler = reinterpret_cast<uart_handler_t*>(handle);
    uart_handler->register_handler(msg_type, handler);
}

void uart_handler_register_dest_handler(uart_handler_handle_t handle,
                                         msg_destination_t destination,
                                         uart_message_handler_cb_t handler)
{
    if (handle == nullptr || handler == nullptr)
    {
        return;
    }
    
    uart_handler_t* uart_handler = reinterpret_cast<uart_handler_t*>(handle);
    uart_handler->register_handler(destination, handler);
}

int uart_handler_is_initialized(uart_handler_handle_t handle)
{
    if (handle == nullptr)
    {
        return 0;
    }
    
    uart_handler_t* handler = reinterpret_cast<uart_handler_t*>(handle);
    return handler->is_initialized() ? 1 : 0;
}

} // extern "C"

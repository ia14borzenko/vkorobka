#pragma once

#include "message_protocol.h"
#include "my_types.h"

// Обработка команды TEST
void command_handle_test();

// Обработка команды STATUS
void command_handle_status();

// Обработка команд текстинга (TEXT_CLEAR, TEXT_ADD)
void command_handle_texting(const char* json_buf, u32 json_len);

// Обработка данных изображения (JPG echo)
void command_handle_image_data(const u8* payload, u32 payload_len);

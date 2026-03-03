#pragma once

#include "message_protocol.h"
#include "my_types.h"
#include "netpack.h"

// Обработчик новых сообщений через message_bridge
void message_dispatcher_handle_new_message(const msg_header_t* header, const u8* payload, u32 payload_len);

// Обработчик принятых пакетов (старый формат - больше не используется)
void message_dispatcher_handle_packet(cmdcode_t cmd_code, const char* payload, u32 payload_len);

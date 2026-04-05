#pragma once

#include "my_types.h"
#include <stdbool.h>

// Запуск FreeRTOS-задачи: I2S RX (INMP441) → MSG_TYPE_STREAM на TCP (если соединение есть).
// Вызывать после инициализации message_bridge и TCP, когда сеть нужна (#ifndef LCD_DEBUG_NO_NET).
void mic_stream_start(void);

// Управление передачей PCM на хост (по командам voice.on / voice.off с pyapp).
void mic_stream_set_tx_enabled(bool enabled);

bool mic_stream_is_tx_enabled(void);

// JSON: rate_hz, bits (16|24), gain_db, chunk_samples (64..512), mute (bool), clip (bool).
// Только когда voice.off; иначе false и конфиг не меняется.
bool mic_stream_set_config_json(const char* json_object);

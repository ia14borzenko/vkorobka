#pragma once

// Запуск FreeRTOS-задачи: I2S RX (INMP441) → MSG_TYPE_STREAM на TCP (если соединение есть).
// Вызывать после инициализации message_bridge и TCP, когда сеть нужна (#ifndef LCD_DEBUG_NO_NET).
void mic_stream_start(void);

// Управление передачей PCM на хост (по командам voice.on / voice.off с pyapp).
void mic_stream_set_tx_enabled(bool enabled);

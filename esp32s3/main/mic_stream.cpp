#include "mic_stream.hpp"
#include "mic_pins.hpp"
#include "message_bridge.hpp"
#include "message_protocol.h"
#include "my_types.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

static const char* TAG = "mic_stream";

extern message_bridge_t* g_message_bridge;

// Совмещаемся с stream_handler: 1 = кадр LCD; 2 = PCM с микрофона
static constexpr u16 MIC_STREAM_ID = 2;
static constexpr u32 MIC_SAMPLE_RATE_HZ = 48000;
// После декодирования стерео — столько моно-сэмплов (левый канал) в одном пакете
static constexpr size_t MIC_MONO_SAMPLES_PER_PACKET = 512;
// I2S RX: стерео кадр = 2×int32 (L, R); INMP441 с L/R→GND отдаёт данные в левом слоте
static constexpr size_t MIC_RAW_INT32_PER_READ =
    MIC_MONO_SAMPLES_PER_PACKET * 2;
static constexpr size_t BYTES_PER_SAMPLE = 3;  // 24-bit packed LE

// +3 dB по амплитуде: × 10^(3/20) ≈ 1.412538 (целочисленно, без libm)
static constexpr s64 MIC_GAIN_NUM = 1412538;
static constexpr s64 MIC_GAIN_DEN = 1000000;

static int32_t apply_mic_gain_s24(int32_t s24)
{
    s64 y = (s64)s24 * MIC_GAIN_NUM / MIC_GAIN_DEN;
    if (y > 0x7fffff)
    {
        y = 0x7fffff;
    }
    if (y < -0x800000)
    {
        y = -0x800000;
    }
    return (int32_t)y;
}

static void pack_i24_le(int32_t s24, uint8_t* p)
{
    if (s24 > 0x7fffff)
    {
        s24 = 0x7fffff;
    }
    if (s24 < -0x7fffff)
    {
        s24 = -0x7fffff;
    }
    p[0] = (uint8_t)(s24 & 0xFF);
    p[1] = (uint8_t)((s24 >> 8) & 0xFF);
    p[2] = (uint8_t)((s24 >> 16) & 0xFF);
}

/** 32-бит слот I2S → 24-бит signed (старшие значащие биты после сдвига на 8). */
static int32_t i32_slot_to_s24(int32_t raw)
{
    return raw >> 8;
}

struct __attribute__((packed)) MicPcmStreamHeader
{
    u32 sample_rate_hz;
    u16 sample_count;
    u16 bits_per_sample;  // 24
};

static i2s_chan_handle_t s_rx = nullptr;
static u8 s_seq = 0;
static std::atomic<bool> s_tx_enabled{false};

void mic_stream_set_tx_enabled(bool enabled)
{
    s_tx_enabled.store(enabled, std::memory_order_relaxed);
    ESP_LOGI(TAG, "TCP stream TX %s", enabled ? "ON" : "OFF");
}

static esp_err_t mic_i2s_init(void)
{
    // Явно I2S0: второй периферийный порт (динамик) — I2S1, иначе предупреждение
    // «controller 0 has been occupied» и треск на выходе.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &s_rx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel: 0x%x", err);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)MIC_SAMPLE_RATE_HZ),
        // Стерео: иначе на части плат mono+LEFT даёт «тишину» и треск; берём только L.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_GPIO_BCLK,
            .ws = MIC_I2S_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: 0x%x", err);
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_enable: 0x%x", err);
        i2s_del_channel(s_rx);
        s_rx = nullptr;
        return err;
    }
    return ESP_OK;
}

static void mic_stream_task(void* arg)
{
    (void)arg;

    const size_t raw_bytes = MIC_RAW_INT32_PER_READ * sizeof(int32_t);
    int32_t* raw = (int32_t*)heap_caps_malloc(raw_bytes, MALLOC_CAP_DEFAULT);
    if (!raw)
    {
        ESP_LOGE(TAG, "raw buffer alloc failed");
        vTaskDelete(nullptr);
        return;
    }

    const size_t payload_len =
        sizeof(MicPcmStreamHeader) + MIC_MONO_SAMPLES_PER_PACKET * BYTES_PER_SAMPLE;
    u8* payload = (u8*)heap_caps_malloc(payload_len, MALLOC_CAP_DEFAULT);
    if (!payload)
    {
        ESP_LOGE(TAG, "payload buffer alloc failed");
        heap_caps_free(raw);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "task running: %u Hz, stereo RX → %u mono L/chunk, 24-bit",
             (unsigned)MIC_SAMPLE_RATE_HZ, (unsigned)MIC_MONO_SAMPLES_PER_PACKET);

    while (1)
    {
        size_t nread = 0;
        esp_err_t err = i2s_channel_read(s_rx, raw, raw_bytes, &nread, portMAX_DELAY);
        if (err != ESP_OK || nread < 2 * sizeof(int32_t))
        {
            ESP_LOGW(TAG, "i2s_channel_read: err=0x%x nread=%u", err, (unsigned)nread);
            continue;
        }

        const size_t nstereo = nread / (2 * sizeof(int32_t));
        const size_t nsamp = nstereo;  // моно: один сэмпл на стерео-кадр

        uint8_t* pcm24 = payload + sizeof(MicPcmStreamHeader);
        for (size_t f = 0; f < nsamp; ++f)
        {
            const int32_t left = raw[2 * f];
            const int32_t s24 = i32_slot_to_s24(left);
            pack_i24_le(apply_mic_gain_s24(s24), pcm24 + f * BYTES_PER_SAMPLE);
        }

        MicPcmStreamHeader* hdr = (MicPcmStreamHeader*)payload;
        hdr->sample_rate_hz = MIC_SAMPLE_RATE_HZ;
        hdr->sample_count = (u16)nsamp;
        hdr->bits_per_sample = 24;

        const size_t actual_payload =
            sizeof(MicPcmStreamHeader) + nsamp * BYTES_PER_SAMPLE;

        if (!s_tx_enabled.load(std::memory_order_relaxed))
        {
            continue;
        }

        if (g_message_bridge)
        {
            msg_header_t h = msg_create_header(
                MSG_TYPE_STREAM,
                MSG_SRC_ESP32,
                MSG_DST_EXTERNAL,
                200,
                MIC_STREAM_ID,
                (u32)actual_payload,
                s_seq++,
                MSG_ROUTE_NONE);

            (void)g_message_bridge->send_message(h, payload, (u32)actual_payload);
        }
    }
}

void mic_stream_start(void)
{
    if (s_rx != nullptr)
    {
        ESP_LOGW(TAG, "already started");
        return;
    }

    esp_err_t e = mic_i2s_init();
    if (e != ESP_OK)
    {
        ESP_LOGE(TAG, "mic_i2s_init failed: 0x%x", e);
        return;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        mic_stream_task,
        "mic_stream",
        4096,
        nullptr,
        5,
        nullptr,
        tskNO_AFFINITY);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = nullptr;
    }
}

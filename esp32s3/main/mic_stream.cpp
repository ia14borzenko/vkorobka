#include "mic_stream.hpp"
#include "mic_pins.hpp"
#include "message_bridge.hpp"
#include "message_protocol.h"
#include "my_types.h"
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cmath>
#include <cstring>

static const char* TAG = "mic_stream";

extern message_bridge_t* g_message_bridge;

static constexpr u16 MIC_STREAM_ID = 2;
static constexpr u16 MIC_CHUNK_MIN = 64;
static constexpr u16 MIC_CHUNK_MAX = 512;
static constexpr u32 MIC_RATE_MIN_HZ = 8000;
static constexpr u32 MIC_RATE_MAX_HZ = 96000;

struct MicStreamConfig
{
    u32 rate_hz = 48000;
    u16 bits = 24;  // 16 or 24
    u16 chunk_samples = 512;
    float gain_db = 3.0f;
    bool mute = false;
    bool clip = true;
};

struct __attribute__((packed)) MicPcmStreamHeader
{
    u32 sample_rate_hz;
    u16 sample_count;
    u16 bits_per_sample;
};

static i2s_chan_handle_t s_rx = nullptr;
static u8 s_seq = 0;
static std::atomic<bool> s_tx_enabled{false};

static portMUX_TYPE s_cfg_mu = portMUX_INITIALIZER_UNLOCKED;
static MicStreamConfig s_active_cfg;
static MicStreamConfig s_pending_cfg;
static std::atomic<bool> s_apply_pending{false};
static u32 s_i2s_rate_hz = 0;  // last programmed I2S clock

static int32_t* s_raw = nullptr;
static u8* s_payload = nullptr;
static size_t s_raw_bytes = 0;
static size_t s_payload_alloc = 0;

void mic_stream_set_tx_enabled(bool enabled)
{
    s_tx_enabled.store(enabled, std::memory_order_relaxed);
    ESP_LOGI(TAG, "TCP stream TX %s", enabled ? "ON" : "OFF");
}

bool mic_stream_is_tx_enabled(void)
{
    return s_tx_enabled.load(std::memory_order_relaxed);
}

static void mic_free_buffers(void)
{
    if (s_raw)
    {
        heap_caps_free(s_raw);
        s_raw = nullptr;
    }
    if (s_payload)
    {
        heap_caps_free(s_payload);
        s_payload = nullptr;
    }
    s_raw_bytes = 0;
    s_payload_alloc = 0;
}

static esp_err_t mic_alloc_buffers(const MicStreamConfig& c)
{
    mic_free_buffers();
    const size_t raw_b = (size_t)c.chunk_samples * 2u * sizeof(int32_t);
    const size_t pcm_b = (size_t)c.chunk_samples * (c.bits == 24 ? 3u : 2u);
    const size_t pay_b = sizeof(MicPcmStreamHeader) + pcm_b;
    s_raw = (int32_t*)heap_caps_malloc(raw_b, MALLOC_CAP_DEFAULT);
    s_payload = (u8*)heap_caps_malloc(pay_b, MALLOC_CAP_DEFAULT);
    if (!s_raw || !s_payload)
    {
        ESP_LOGE(TAG, "buffer alloc failed");
        mic_free_buffers();
        return ESP_ERR_NO_MEM;
    }
    s_raw_bytes = raw_b;
    s_payload_alloc = pay_b;
    return ESP_OK;
}

static void pack_i24_le(int32_t s24, uint8_t* p)
{
    if (s24 > 0x7fffff)
    {
        s24 = 0x7fffff;
    }
    if (s24 < -0x800000)
    {
        s24 = -0x800000;
    }
    p[0] = (uint8_t)(s24 & 0xFF);
    p[1] = (uint8_t)((s24 >> 8) & 0xFF);
    p[2] = (uint8_t)((s24 >> 16) & 0xFF);
}

static int32_t i32_slot_to_s24(int32_t raw)
{
    return raw >> 8;
}

static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

static int32_t apply_gain_clip_s24(float s, float linear_gain, bool doclip)
{
    float y = s * linear_gain;
    if (doclip)
    {
        if (y > 8388607.0f)
        {
            y = 8388607.0f;
        }
        if (y < -8388608.0f)
        {
            y = -8388608.0f;
        }
    }
    return (int32_t)lrintf(y);
}

static int16_t apply_gain_clip_s16(float s, float linear_gain, bool doclip)
{
    float y = s * linear_gain;
    if (doclip)
    {
        if (y > 32767.0f)
        {
            y = 32767.0f;
        }
        if (y < -32768.0f)
        {
            y = -32768.0f;
        }
    }
    return (int16_t)lrintf(y);
}

static esp_err_t mic_i2s_init_rate(u32 rate_hz)
{
    if (s_rx != nullptr)
    {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = nullptr;
    }

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
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate_hz),
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
    s_i2s_rate_hz = rate_hz;
    ESP_LOGI(TAG, "I2S RX rate=%u Hz", (unsigned)rate_hz);
    return ESP_OK;
}

static bool mic_apply_pending_locked(void)
{
    if (!s_apply_pending.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (s_tx_enabled.load(std::memory_order_relaxed))
    {
        return false;
    }

    MicStreamConfig want;
    portENTER_CRITICAL(&s_cfg_mu);
    want = s_pending_cfg;
    portEXIT_CRITICAL(&s_cfg_mu);

    if (want.rate_hz < MIC_RATE_MIN_HZ || want.rate_hz > MIC_RATE_MAX_HZ)
    {
        ESP_LOGE(TAG, "bad rate %u", (unsigned)want.rate_hz);
        portENTER_CRITICAL(&s_cfg_mu);
        s_apply_pending.store(false, std::memory_order_relaxed);
        portEXIT_CRITICAL(&s_cfg_mu);
        return true;
    }
    if (want.bits != 16 && want.bits != 24)
    {
        ESP_LOGE(TAG, "bad bits %u", (unsigned)want.bits);
        portENTER_CRITICAL(&s_cfg_mu);
        s_apply_pending.store(false, std::memory_order_relaxed);
        portEXIT_CRITICAL(&s_cfg_mu);
        return true;
    }
    if (want.chunk_samples < MIC_CHUNK_MIN || want.chunk_samples > MIC_CHUNK_MAX)
    {
        ESP_LOGE(TAG, "bad chunk_samples %u", (unsigned)want.chunk_samples);
        portENTER_CRITICAL(&s_cfg_mu);
        s_apply_pending.store(false, std::memory_order_relaxed);
        portEXIT_CRITICAL(&s_cfg_mu);
        return true;
    }

    if (s_i2s_rate_hz != want.rate_hz)
    {
        if (mic_i2s_init_rate(want.rate_hz) != ESP_OK)
        {
            ESP_LOGE(TAG, "I2S reinit failed");
            portENTER_CRITICAL(&s_cfg_mu);
            s_apply_pending.store(false, std::memory_order_relaxed);
            portEXIT_CRITICAL(&s_cfg_mu);
            return true;
        }
    }

    if (mic_alloc_buffers(want) != ESP_OK)
    {
        portENTER_CRITICAL(&s_cfg_mu);
        s_apply_pending.store(false, std::memory_order_relaxed);
        portEXIT_CRITICAL(&s_cfg_mu);
        return true;
    }

    portENTER_CRITICAL(&s_cfg_mu);
    s_active_cfg = want;
    s_apply_pending.store(false, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_cfg_mu);
    ESP_LOGI(TAG, "config: %u Hz, %u-bit, chunk=%u, gain=%.2f dB, mute=%d, clip=%d",
             (unsigned)want.rate_hz, (unsigned)want.bits, (unsigned)want.chunk_samples,
             want.gain_db, want.mute ? 1 : 0, want.clip ? 1 : 0);
    return true;
}

static void mic_stream_task(void* arg)
{
    (void)arg;

    while (1)
    {
        (void)mic_apply_pending_locked();

        MicStreamConfig cfg;
        portENTER_CRITICAL(&s_cfg_mu);
        cfg = s_active_cfg;
        portEXIT_CRITICAL(&s_cfg_mu);

        if (!s_rx || !s_raw || !s_payload || s_raw_bytes == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const size_t expect_read = (size_t)cfg.chunk_samples * 2u * sizeof(int32_t);
        if (expect_read > s_raw_bytes)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t nread = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_raw, expect_read, &nread, portMAX_DELAY);
        if (err != ESP_OK || nread < 2 * sizeof(int32_t))
        {
            ESP_LOGW(TAG, "i2s_channel_read: err=0x%x nread=%u", err, (unsigned)nread);
            continue;
        }

        const size_t nstereo = nread / (2 * sizeof(int32_t));
        const size_t nsamp = nstereo;
        const float g_lin = db_to_linear(cfg.gain_db);

        uint8_t* pcm_out = s_payload + sizeof(MicPcmStreamHeader);
        const unsigned bps = cfg.bits;

        for (size_t f = 0; f < nsamp; ++f)
        {
            int32_t s24 = i32_slot_to_s24(s_raw[2 * f]);
            if (cfg.mute)
            {
                s24 = 0;
            }
            else if (bps == 24)
            {
                s24 = apply_gain_clip_s24((float)s24, g_lin, cfg.clip);
                pack_i24_le(s24, pcm_out + f * 3u);
            }
            else
            {
                float s = (float)s24;
                int16_t s16 = apply_gain_clip_s16(s, g_lin, cfg.clip);
                pcm_out[f * 2u] = (uint8_t)(s16 & 0xFF);
                pcm_out[f * 2u + 1] = (uint8_t)((s16 >> 8) & 0xFF);
            }
        }

        MicPcmStreamHeader* hdr = (MicPcmStreamHeader*)s_payload;
        hdr->sample_rate_hz = cfg.rate_hz;
        hdr->sample_count = (u16)nsamp;
        hdr->bits_per_sample = bps;

        const size_t actual_payload =
            sizeof(MicPcmStreamHeader) + nsamp * (bps == 24 ? 3u : 2u);

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

            (void)g_message_bridge->send_message(h, s_payload, (u32)actual_payload);
        }
    }
}

void mic_stream_start(void)
{
    static bool s_task_started = false;
    if (s_task_started)
    {
        ESP_LOGW(TAG, "task already started");
        return;
    }

    s_pending_cfg = MicStreamConfig{};
    s_active_cfg = MicStreamConfig{};

    if (mic_i2s_init_rate(s_active_cfg.rate_hz) != ESP_OK)
    {
        ESP_LOGE(TAG, "mic_i2s_init_rate failed");
        return;
    }

    if (mic_alloc_buffers(s_active_cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "mic_alloc_buffers failed");
        if (s_rx)
        {
            i2s_channel_disable(s_rx);
            i2s_del_channel(s_rx);
            s_rx = nullptr;
        }
        return;
    }
    s_apply_pending.store(false, std::memory_order_relaxed);

    const BaseType_t ok = xTaskCreatePinnedToCore(
        mic_stream_task,
        "mic_stream",
        6144,
        nullptr,
        5,
        nullptr,
        tskNO_AFFINITY);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed");
        mic_free_buffers();
        if (s_rx)
        {
            i2s_channel_disable(s_rx);
            i2s_del_channel(s_rx);
            s_rx = nullptr;
        }
        return;
    }
    s_task_started = true;
    ESP_LOGI(TAG, "started");
}

static bool parse_u32(const cJSON* o, const char* key, u32* out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || !cJSON_IsNumber(it))
    {
        return false;
    }
    *out = (u32)it->valuedouble;
    return true;
}

static bool parse_u16_loose(const cJSON* o, const char* key, u16* out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || !cJSON_IsNumber(it))
    {
        return false;
    }
    *out = (u16)it->valuedouble;
    return true;
}

static bool parse_float_item(const cJSON* o, const char* key, float* out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it || !cJSON_IsNumber(it))
    {
        return false;
    }
    *out = (float)it->valuedouble;
    return true;
}

static bool parse_bool_item(const cJSON* o, const char* key, bool* out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it)
    {
        return false;
    }
    if (cJSON_IsBool(it))
    {
        *out = cJSON_IsTrue(it);
        return true;
    }
    if (cJSON_IsNumber(it))
    {
        *out = it->valuedouble != 0.0;
        return true;
    }
    return false;
}

bool mic_stream_set_config_json(const char* json_object)
{
    if (mic_stream_is_tx_enabled())
    {
        ESP_LOGW(TAG, "voice.set rejected: stream active");
        return false;
    }

    cJSON* root = cJSON_Parse(json_object);
    if (!root || !cJSON_IsObject(root))
    {
        ESP_LOGE(TAG, "voice.set: invalid JSON");
        if (root)
        {
            cJSON_Delete(root);
        }
        return false;
    }

    portENTER_CRITICAL(&s_cfg_mu);
    MicStreamConfig c = s_active_cfg;
    portEXIT_CRITICAL(&s_cfg_mu);

    u32 u32v = 0;
    u16 u16v = 0;
    float fv = 0.f;
    bool bv = false;

    if (parse_u32(root, "rate_hz", &u32v))
    {
        c.rate_hz = u32v;
    }
    if (parse_u16_loose(root, "bits", &u16v))
    {
        c.bits = u16v;
    }
    if (parse_u16_loose(root, "chunk_samples", &u16v))
    {
        c.chunk_samples = u16v;
    }
    if (parse_float_item(root, "gain_db", &fv))
    {
        c.gain_db = fv;
    }
    if (parse_bool_item(root, "mute", &bv))
    {
        c.mute = bv;
    }
    if (parse_bool_item(root, "clip", &bv))
    {
        c.clip = bv;
    }

    cJSON_Delete(root);

    if (c.rate_hz < MIC_RATE_MIN_HZ || c.rate_hz > MIC_RATE_MAX_HZ)
    {
        ESP_LOGE(TAG, "voice.set: rate_hz out of range");
        return false;
    }
    if (c.bits != 16 && c.bits != 24)
    {
        ESP_LOGE(TAG, "voice.set: bits must be 16 or 24");
        return false;
    }
    if (c.chunk_samples < MIC_CHUNK_MIN || c.chunk_samples > MIC_CHUNK_MAX)
    {
        ESP_LOGE(TAG, "voice.set: chunk_samples must be %u..%u", (unsigned)MIC_CHUNK_MIN,
                 (unsigned)MIC_CHUNK_MAX);
        return false;
    }

    portENTER_CRITICAL(&s_cfg_mu);
    s_pending_cfg = c;
    s_apply_pending.store(true, std::memory_order_relaxed);
    portEXIT_CRITICAL(&s_cfg_mu);

    ESP_LOGI(TAG, "voice.set queued");
    return true;
}

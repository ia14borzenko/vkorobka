#include "dyn_playback.hpp"
#include "dyn_pins.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include <atomic>
#include <string.h>

static const char* TAG = "dyn_playback";

// 22050 Гц, 16 bit — меньше трафика и реже чанки/сек относительно 48k/24bit.
// В даташите MAX98357 22.05 kHz не в списке «поддерживаемых» LRCLK; при сбоях см. 16/32 kHz.
static constexpr u32 DYN_SAMPLE_RATE_HZ = 22050;
static constexpr u16 DYN_BITS_PER_SAMPLE = 16;
static constexpr size_t BYTES_PER_PCM16 = 2;
static constexpr u16 DYN_MAX_MONO_SAMPLES = 512;
static constexpr u32 QUEUE_DEPTH = 16;
static constexpr size_t SILENCE_STEREO_FRAMES_ON_OFF = 256;

struct __attribute__((packed)) PcmStreamHeader
{
    u32 sample_rate_hz;
    u16 sample_count;
    u16 bits_per_sample;
};

struct DynPcmChunk
{
    u16 n_stereo_frames;
    u16 _pad;
};

static int32_t* chunk_samples(DynPcmChunk* c)
{
    return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(c) + sizeof(DynPcmChunk));
}

static i2s_chan_handle_t s_tx = nullptr;
static QueueHandle_t s_queue = nullptr;
static std::atomic<bool> s_armed{false};

static size_t chunk_alloc_bytes(u16 stereo_frames)
{
    return sizeof(DynPcmChunk) + (size_t)stereo_frames * 2u * sizeof(int32_t);
}

/** int16 LE → значение для 32-bit Philips-слота (старшие 16 бит). */
static int32_t i16_to_i32_slot(int16_t s)
{
    return (int32_t)s << 16;
}

static void sd_mode_shutdown(void)
{
    gpio_set_level(DYN_GPIO_SD_MODE, 0);
}

static void sd_mode_run(void)
{
    gpio_set_level(DYN_GPIO_SD_MODE, 1);
}

static void drain_queue_free(void)
{
    if (!s_queue)
    {
        return;
    }
    DynPcmChunk* c = nullptr;
    while (xQueueReceive(s_queue, &c, 0) == pdTRUE)
    {
        if (c)
        {
            heap_caps_free(c);
        }
    }
}

static void write_silence_stereo(size_t stereo_frames)
{
    if (!s_tx || stereo_frames == 0)
    {
        return;
    }
    const size_t bytes = stereo_frames * 2u * sizeof(int32_t);
    int32_t* z = (int32_t*)heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    if (!z)
    {
        return;
    }
    memset(z, 0, bytes);
    size_t wrote = 0;
    (void)i2s_channel_write(s_tx, z, bytes, &wrote, pdMS_TO_TICKS(500));
    heap_caps_free(z);
}

static esp_err_t dyn_i2s_tx_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel tx: 0x%x", err);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DYN_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = DYN_I2S_GPIO_BCLK,
            .ws = DYN_I2S_GPIO_WS,
            .dout = DYN_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode tx: 0x%x", err);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return err;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_channel_enable tx: 0x%x", err);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return err;
    }
    return ESP_OK;
}

static void enqueue_chunk_drop_oldest(DynPcmChunk* chunk)
{
    if (!s_queue || !chunk)
    {
        return;
    }
    while (xQueueSend(s_queue, &chunk, 0) != pdTRUE)
    {
        DynPcmChunk* old = nullptr;
        if (xQueueReceive(s_queue, &old, 0) != pdTRUE)
        {
            heap_caps_free(chunk);
            return;
        }
        if (old)
        {
            heap_caps_free(old);
        }
    }
}

static void dyn_playback_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "playback task running");

    for (;;)
    {
        DynPcmChunk* chunk = nullptr;
        if (xQueueReceive(s_queue, &chunk, pdMS_TO_TICKS(20)) != pdTRUE || chunk == nullptr)
        {
            if (s_armed.load(std::memory_order_relaxed) && s_tx)
            {
                static int32_t zero_pair[2] = {0, 0};
                size_t w = 0;
                (void)i2s_channel_write(s_tx, zero_pair, sizeof(zero_pair), &w, pdMS_TO_TICKS(40));
            }
            continue;
        }

        const size_t nbytes = (size_t)chunk->n_stereo_frames * 2u * sizeof(int32_t);
        const int32_t* p = chunk_samples(chunk);
        size_t remaining = nbytes;
        while (remaining > 0 && s_tx)
        {
            size_t wrote = 0;
            esp_err_t e = i2s_channel_write(s_tx, p, remaining, &wrote, portMAX_DELAY);
            if (e != ESP_OK)
            {
                ESP_LOGW(TAG, "i2s_channel_write: 0x%x", e);
                break;
            }
            if (wrote == 0)
            {
                break;
            }
            remaining -= wrote;
            p = (const int32_t*)((const uint8_t*)p + wrote);
        }
        heap_caps_free(chunk);
    }
}

void dyn_playback_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << DYN_GPIO_SD_MODE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    sd_mode_shutdown();

    if (dyn_i2s_tx_init() != ESP_OK)
    {
        return;
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(DynPcmChunk*));
    if (!s_queue)
    {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    if (xTaskCreatePinnedToCore(dyn_playback_task, "dyn_playback", 4096, nullptr, 6, nullptr,
                                tskNO_AFFINITY) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
    }
}

void dyn_playback_feed(const u8* payload, u32 payload_len)
{
    if (!s_tx || !s_queue || !payload || !s_armed.load(std::memory_order_relaxed))
    {
        return;
    }

    const u32 hdr_sz = (u32)sizeof(PcmStreamHeader);
    if (payload_len < hdr_sz)
    {
        return;
    }

    const PcmStreamHeader* hdr = (const PcmStreamHeader*)payload;
    if (hdr->sample_rate_hz != DYN_SAMPLE_RATE_HZ || hdr->bits_per_sample != DYN_BITS_PER_SAMPLE ||
        hdr->sample_count == 0 || hdr->sample_count > DYN_MAX_MONO_SAMPLES)
    {
        ESP_LOGW(TAG, "bad hdr: rate=%u bits=%u n=%u (expect %u Hz, %u-bit)",
                 (unsigned)hdr->sample_rate_hz, (unsigned)hdr->bits_per_sample,
                 (unsigned)hdr->sample_count, (unsigned)DYN_SAMPLE_RATE_HZ,
                 (unsigned)DYN_BITS_PER_SAMPLE);
        return;
    }

    const u32 pcm_bytes = (u32)hdr->sample_count * BYTES_PER_PCM16;
    if (payload_len < hdr_sz + pcm_bytes)
    {
        ESP_LOGW(TAG, "short payload: %u < %u", (unsigned)payload_len,
                 (unsigned)(hdr_sz + pcm_bytes));
        return;
    }

    const u16 n = hdr->sample_count;
    const size_t alloc_b = chunk_alloc_bytes(n);
    DynPcmChunk* chunk = (DynPcmChunk*)heap_caps_malloc(alloc_b, MALLOC_CAP_DEFAULT);
    if (!chunk)
    {
        ESP_LOGE(TAG, "chunk alloc failed");
        return;
    }
    chunk->n_stereo_frames = n;
    chunk->_pad = 0;

    int32_t* out = chunk_samples(chunk);
    const uint8_t* pcm = payload + hdr_sz;
    for (u16 i = 0; i < n; ++i)
    {
        int16_t s16 = (int16_t)(uint16_t)(pcm[(size_t)i * BYTES_PER_PCM16] |
                                          ((uint16_t)pcm[(size_t)i * BYTES_PER_PCM16 + 1] << 8));
        int32_t slot = i16_to_i32_slot(s16);
        out[2 * i] = slot;
        out[2 * i + 1] = slot;
    }

    enqueue_chunk_drop_oldest(chunk);
}

void dyn_playback_set_armed(bool armed)
{
    if (armed)
    {
        drain_queue_free();
        s_armed.store(true, std::memory_order_relaxed);
        sd_mode_run();
        ESP_LOGI(TAG, "armed ON");
    }
    else
    {
        s_armed.store(false, std::memory_order_relaxed);
        drain_queue_free();
        write_silence_stereo(SILENCE_STEREO_FRAMES_ON_OFF);
        sd_mode_shutdown();
        ESP_LOGI(TAG, "armed OFF");
    }
}

bool dyn_playback_is_armed(void)
{
    return s_armed.load(std::memory_order_relaxed);
}

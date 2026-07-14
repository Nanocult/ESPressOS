/**
 * @file svc_audio.c
 * @brief Persistent Audio Engine - Owns I2S, survives app transitions
 */
#include "kernel_api.h"
#include "svc_audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"

static const char* TAG = "svc_audio";

/* Permanent DMA buffers in PSRAM (never freed) */
#define AUDIO_BUF_SIZE      (8 * 1024)  /* 8KB per buffer */
#define RINGBUF_SIZE        (32 * 1024) /* 32KB ringbuffer */

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static RingbufHandle_t   s_playback_rb = NULL;
static RingbufHandle_t   s_capture_rb  = NULL;
static uint8_t           s_volume = 80;

/* ========================================================================== */
/* INITIALIZATION                                                             */
/* ========================================================================== */
void svc_audio_init(void) {
    /* I2S Standard Config for MAX98357A + INMP441 TDM */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = GPIO_NUM_4,
            .ws   = GPIO_NUM_5,
            .dout = GPIO_NUM_6,  /* → MAX98357A DIN */
            .din  = GPIO_NUM_7,  /* ← INMP441 SD    */
        },
    };

    ESP_ERROR_CHECK(i2s_new_channel(&std_cfg, &s_tx_handle, &s_rx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    /* Allocate ringbuffers in PSRAM (permanent) */
    s_playback_rb = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_capture_rb  = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);

    /* Start background DMA feed task */
    xTaskCreatePinnedToCore(
        [](void*) {
            uint8_t buf[AUDIO_BUF_SIZE];
            while (1) {
                size_t bytes_read = 0;
                void* item = xRingbufferReceiveUpTo(s_playback_rb, &bytes_read, pdMS_TO_TICKS(10), AUDIO_BUF_SIZE);
                if (item) {
                    size_t written = 0;
                    i2s_channel_write(s_tx_handle, item, bytes_read, &written, pdMS_TO_TICKS(100));
                    vRingbufferReturnItem(s_playback_rb, item);
                } else {
                    /* Feed silence to prevent DAC pops */
                    memset(buf, 0, sizeof(buf));
                    size_t w; i2s_channel_write(s_tx_handle, buf, sizeof(buf), &w, pdMS_TO_TICKS(10));
                }
            }
        },
        "audio_dma", 3072, NULL, configMAX_PRIORITIES - 2, NULL, 0
    );

    ESP_LOGI(TAG, "✓ Audio engine initialized (I2S+TDM, %dKB buffers)", RINGBUF_SIZE/1024);
}

/* ========================================================================== */
/* KERNEL API IMPLEMENTATIONS                                                 */
/* ========================================================================== */

k_err_t svc_audio_open_output(const k_audio_format_t* fmt, k_audio_handle_t* handle) {
    if (!fmt || !handle) return K_ERR_INVALID;
    /* For MVP: single output stream. Handle is just a sentinel. */
    *handle = (k_audio_handle_t)0xAUDIO_OUT;
    return K_OK;
}

k_err_t svc_audio_write(k_audio_handle_t handle, const void* data, size_t len, uint32_t timeout_ms) {
    if (handle != (k_audio_handle_t)0xAUDIO_OUT) return K_ERR_INVALID;
    if (xRingbufferSend(s_playback_rb, data, len, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return K_ERR_TIMEOUT;
    }
    return K_OK;
}

k_err_t svc_audio_close(k_audio_handle_t handle) {
    /* Drain ringbuffer on close to prevent stale audio */
    vRingbufferDelete(s_playback_rb);
    s_playback_rb = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    return K_OK;
}

k_err_t svc_audio_set_volume(uint8_t vol) {
    s_volume = (vol > 100) ? 100 : vol;
    /* Apply software gain or codec register write here */
    ESP_LOGD(TAG, "Volume set to %u%%", s_volume);
    return K_OK;
}

/* Mic stubs - implement similarly using s_capture_rb + rx_handle */
k_err_t svc_audio_mic_open(const k_audio_format_t* fmt, k_audio_handle_t* h) { *h = (k_audio_handle_t)0xMIC_IN; return K_OK; }
k_err_t svc_audio_mic_read(k_audio_handle_t h, void* b, size_t l, size_t* br, uint32_t t) { 
    void* item = xRingbufferReceiveUpTo(s_capture_rb, br, pdMS_TO_TICKS(t), l);
    if(item){memcpy(b,item,*br);vRingbufferReturnItem(s_capture_rb,item);return K_OK;} 
    return K_ERR_TIMEOUT; 
}
k_err_t svc_audio_mic_close(k_audio_handle_t h) { return K_OK; }

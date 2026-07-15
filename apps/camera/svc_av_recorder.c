#include "svc_av_recorder.h"
#include "svc_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char* TAG = "svc_av_recorder";

/* Container format constants */
#define ESPAV_MAGIC "ESPAV"
#define ESPAV_VERSION 1
#define CHUNK_TYPE_VIDEO 0x01
#define CHUNK_TYPE_AUDIO 0x02

/* Buffer sizes */
#define VIDEO_QUEUE_SIZE 4
#define AUDIO_QUEUE_SIZE 16
#define AUDIO_CHUNK_SIZE 1024  // ~23ms of 44.1kHz 16-bit mono audio

/* Container header structure */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint8_t video_fps;
    uint32_t audio_sample_rate;
    uint8_t audio_channels;
    uint8_t audio_bits;
    uint8_t reserved[20];
} espav_header_t;

/* Chunk header structure */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;
    uint8_t type;
    uint32_t size;
} espav_chunk_t;

/* Queue item structures */
typedef struct {
    uint32_t timestamp_ms;
    uint8_t* data;
    size_t size;
} video_frame_t;

typedef struct {
    uint32_t timestamp_ms;
    uint8_t* data;
    size_t size;
} audio_chunk_t;

/* Recorder state */
static struct {
    av_recorder_state_t state;
    av_recorder_config_t config;
    FILE* file;
    char path[128];
    
    TaskHandle_t video_task;
    TaskHandle_t audio_task;
    TaskHandle_t writer_task;
    
    QueueHandle_t video_queue;
    QueueHandle_t audio_queue;
    SemaphoreHandle_t stop_mutex;
    
    uint32_t start_time_ms;
    uint32_t frame_count;
    uint32_t audio_chunk_count;
    
    bool stop_requested;
} s_recorder = {
    .state = AV_RECORDER_IDLE,
    .file = NULL,
    .video_task = NULL,
    .audio_task = NULL,
    .writer_task = NULL,
    .video_queue = NULL,
    .audio_queue = NULL,
    .stop_mutex = NULL,
    .stop_requested = false,
};

/* ========================================================================== */
/* UTILITY FUNCTIONS                                                          */
/* ========================================================================== */

static uint32_t get_timestamp_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static k_err_t write_header(void) {
    espav_header_t header = {0};
    memcpy(header.magic, ESPAV_MAGIC, 4);
    header.version = ESPAV_VERSION;
    header.video_fps = s_recorder.config.video_fps;
    header.audio_sample_rate = s_recorder.config.audio_sample_rate;
    header.audio_channels = s_recorder.config.audio_channels;
    header.audio_bits = s_recorder.config.audio_bits;
    
    size_t written = fwrite(&header, 1, sizeof(header), s_recorder.file);
    if (written != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to write header");
        return K_ERR_IO;
    }
    
    fflush(s_recorder.file);
    return K_OK;
}

static k_err_t write_chunk(uint8_t type, uint32_t timestamp_ms, const uint8_t* data, size_t size) {
    espav_chunk_t chunk = {
        .timestamp_ms = timestamp_ms,
        .type = type,
        .size = size,
    };
    
    if (fwrite(&chunk, 1, sizeof(chunk), s_recorder.file) != sizeof(chunk)) {
        return K_ERR_IO;
    }
    
    if (fwrite(data, 1, size, s_recorder.file) != size) {
        return K_ERR_IO;
    }
    
    return K_OK;
}

/* ========================================================================== */
/* VIDEO CAPTURE TASK                                                         */
/* ========================================================================== */

static void video_capture_task(void* arg) {
    ESP_LOGI(TAG, "Video capture task started @ %d fps", s_recorder.config.video_fps);
    
    uint32_t frame_interval_us = 1000000 / s_recorder.config.video_fps;
    uint32_t next_capture_time = esp_timer_get_time();
    
    while (!s_recorder.stop_requested) {
        uint32_t now = esp_timer_get_time();
        
        if (now >= next_capture_time) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb) {
                video_frame_t frame = {
                    .timestamp_ms = get_timestamp_ms() - s_recorder.start_time_ms,
                    .size = fb->len,
                };
                
                frame.data = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
                if (frame.data) {
                    memcpy(frame.data, fb->buf, fb->len);
                    
                    if (xQueueSend(s_recorder.video_queue, &frame, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Video queue full, dropping frame");
                        heap_caps_free(frame.data);
                    }
                }
                
                esp_camera_fb_return(fb);
            }
            
            next_capture_time = now + frame_interval_us;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    ESP_LOGI(TAG, "Video capture task stopped");
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* AUDIO CAPTURE TASK                                                         */
/* ========================================================================== */

static void audio_capture_task(void* arg) {
    ESP_LOGI(TAG, "Audio capture task started @ %d Hz", s_recorder.config.audio_sample_rate);
    
    k_audio_handle_t mic_handle;
    k_audio_format_t fmt = {
        .sample_rate = s_recorder.config.audio_sample_rate,
        .channels = s_recorder.config.audio_channels,
        .bits_per_sample = s_recorder.config.audio_bits,
        .codec = 0, // PCM
    };
    
    if (svc_audio_mic_open(&fmt, &mic_handle) != K_OK) {
        ESP_LOGE(TAG, "Failed to open microphone");
        vTaskDelete(NULL);
        return;
    }
    
    uint8_t* audio_buf = heap_caps_malloc(AUDIO_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        svc_audio_mic_close(mic_handle);
        vTaskDelete(NULL);
        return;
    }
    
    while (!s_recorder.stop_requested) {
        size_t bytes_read = 0;
        k_err_t err = svc_audio_mic_read(mic_handle, audio_buf, AUDIO_CHUNK_SIZE, &bytes_read, 100);
        
        if (err == K_OK && bytes_read > 0) {
            audio_chunk_t chunk = {
                .timestamp_ms = get_timestamp_ms() - s_recorder.start_time_ms,
                .size = bytes_read,
            };
            
            chunk.data = heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
            if (chunk.data) {
                memcpy(chunk.data, audio_buf, bytes_read);
                
                if (xQueueSend(s_recorder.audio_queue, &chunk, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Audio queue full, dropping chunk");
                    heap_caps_free(chunk.data);
                }
            }
        }
    }
    
    heap_caps_free(audio_buf);
    svc_audio_mic_close(mic_handle);
    
    ESP_LOGI(TAG, "Audio capture task stopped");
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* FILE WRITER TASK                                                           */
/* ========================================================================== */

static void file_writer_task(void* arg) {
    ESP_LOGI(TAG, "File writer task started");
    
    while (!s_recorder.stop_requested || uxQueueMessagesWaiting(s_recorder.video_queue) > 0 
                                       || uxQueueMessagesWaiting(s_recorder.audio_queue) > 0) {
        
        // Process video frames
        video_frame_t video_frame;
        while (xQueueReceive(s_recorder.video_queue, &video_frame, 0) == pdTRUE) {
            if (write_chunk(CHUNK_TYPE_VIDEO, video_frame.timestamp_ms, video_frame.data, video_frame.size) == K_OK) {
                s_recorder.frame_count++;
            } else {
                ESP_LOGE(TAG, "Failed to write video frame");
            }
            heap_caps_free(video_frame.data);
        }
        
        // Process audio chunks
        audio_chunk_t audio_chunk;
        while (xQueueReceive(s_recorder.audio_queue, &audio_chunk, 0) == pdTRUE) {
            if (write_chunk(CHUNK_TYPE_AUDIO, audio_chunk.timestamp_ms, audio_chunk.data, audio_chunk.size) == K_OK) {
                s_recorder.audio_chunk_count++;
            } else {
                ESP_LOGE(TAG, "Failed to write audio chunk");
            }
            heap_caps_free(audio_chunk.data);
        }
        
        // Periodic flush to SD card
        if (s_recorder.frame_count % 10 == 0) {
            fflush(s_recorder.file);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Final flush
    fflush(s_recorder.file);
    fclose(s_recorder.file);
    s_recorder.file = NULL;
    
    ESP_LOGI(TAG, "File writer task stopped. Recorded %u frames, %u audio chunks",
             s_recorder.frame_count, s_recorder.audio_chunk_count);
    
    s_recorder.state = AV_RECORDER_IDLE;
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* PUBLIC API                                                                 */
/* ========================================================================== */

k_err_t svc_av_recorder_init(void) {
    if (s_recorder.stop_mutex == NULL) {
        s_recorder.stop_mutex = xSemaphoreCreateMutex();
    }
    
    ESP_LOGI(TAG, "✓ A/V recorder service initialized");
    return K_OK;
}

k_err_t svc_av_recorder_start(const char* path, const av_recorder_config_t* config) {
    if (s_recorder.state != AV_RECORDER_IDLE) {
        ESP_LOGW(TAG, "Recorder already active");
        return K_ERR_BUSY;
    }
    
    xSemaphoreTake(s_recorder.stop_mutex, portMAX_DELAY);
    
    // Validate config
    if (config->video_fps < 5 || config->video_fps > 30) {
        xSemaphoreGive(s_recorder.stop_mutex);
        return K_ERR_INVALID;
    }
    
    // Store config
    s_recorder.config = *config;
    strncpy(s_recorder.path, path, sizeof(s_recorder.path) - 1);
    
    // Open output file
    s_recorder.file = fopen(path, "wb");
    if (!s_recorder.file) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        xSemaphoreGive(s_recorder.stop_mutex);
        return K_ERR_IO;
    }
    
    // Write header
    if (write_header() != K_OK) {
        fclose(s_recorder.file);
        s_recorder.file = NULL;
        xSemaphoreGive(s_recorder.stop_mutex);
        return K_ERR_IO;
    }
    
    // Create queues
    s_recorder.video_queue = xQueueCreate(VIDEO_QUEUE_SIZE, sizeof(video_frame_t));
    s_recorder.audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_chunk_t));
    
    if (!s_recorder.video_queue || !s_recorder.audio_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        fclose(s_recorder.file);
        s_recorder.file = NULL;
        if (s_recorder.video_queue) vQueueDelete(s_recorder.video_queue);
        if (s_recorder.audio_queue) vQueueDelete(s_recorder.audio_queue);
        xSemaphoreGive(s_recorder.stop_mutex);
        return K_ERR_NOMEM;
    }
    
    // Reset counters
    s_recorder.frame_count = 0;
    s_recorder.audio_chunk_count = 0;
    s_recorder.stop_requested = false;
    s_recorder.start_time_ms = get_timestamp_ms();
    
    // Start capture tasks
    BaseType_t ret1 = xTaskCreatePinnedToCore(video_capture_task, "av_video", 4096, NULL, 5, &s_recorder.video_task, 1);
    BaseType_t ret2 = xTaskCreatePinnedToCore(audio_capture_task, "av_audio", 4096, NULL, 5, &s_recorder.audio_task, 1);
    BaseType_t ret3 = xTaskCreatePinnedToCore(file_writer_task, "av_writer", 4096, NULL, 4, &s_recorder.writer_task, 0);
    
    if (ret1 != pdPASS || ret2 != pdPASS || ret3 != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tasks");
        s_recorder.stop_requested = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        fclose(s_recorder.file);
        s_recorder.file = NULL;
        vQueueDelete(s_recorder.video_queue);
        vQueueDelete(s_recorder.audio_queue);
        xSemaphoreGive(s_recorder.stop_mutex);
        return K_ERR_IO;
    }
    
    s_recorder.state = AV_RECORDER_RECORDING;
    xSemaphoreGive(s_recorder.stop_mutex);
    
    ESP_LOGI(TAG, "✓ Recording started: %s", path);
    return K_OK;
}

k_err_t svc_av_recorder_stop(void) {
    if (s_recorder.state != AV_RECORDER_RECORDING) {
        return K_OK;
    }
    
    xSemaphoreTake(s_recorder.stop_mutex, portMAX_DELAY);
    
    ESP_LOGI(TAG, "Stopping recorder...");
    s_recorder.stop_requested = true;
    s_recorder.state = AV_RECORDER_STOPPING;
    
    // Wait for tasks to finish
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Cleanup queues
    if (s_recorder.video_queue) {
        vQueueDelete(s_recorder.video_queue);
        s_recorder.video_queue = NULL;
    }
    if (s_recorder.audio_queue) {
        vQueueDelete(s_recorder.audio_queue);
        s_recorder.audio_queue = NULL;
    }
    
    s_recorder.video_task = NULL;
    s_recorder.audio_task = NULL;
    s_recorder.writer_task = NULL;
    
    xSemaphoreGive(s_recorder.stop_mutex);
    
    ESP_LOGI(TAG, "✓ Recording stopped: %s", s_recorder.path);
    return K_OK;
}

av_recorder_state_t svc_av_recorder_get_state(void) {
    return s_recorder.state;
}

uint32_t svc_av_recorder_get_duration_ms(void) {
    if (s_recorder.state == AV_RECORDER_IDLE) {
        return 0;
    }
    return get_timestamp_ms() - s_recorder.start_time_ms;
}

uint32_t svc_av_recorder_get_frame_count(void) {
    return s_recorder.frame_count;
}

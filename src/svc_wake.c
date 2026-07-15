/**
 * @file svc_wake.c
 * @brief Persistent Wake Word Detection Service
 */
#include "svc_wake.h"
#include "svc_audio.h"
#include "esp_log.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "svc_wake";

static esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t  *s_afe_data = NULL;
static TaskHandle_t s_wake_task = NULL;
static volatile bool s_wake_triggered = false;

/* ========================================================================== */
/* KERNEL STATE & AUTO-LAUNCH                                                 */
/* ========================================================================== */
extern void svc_sys_request_launch(const char* app_name);
extern void svc_audio_play_system_chime(void); /* Plays a short "ding" PCM */

bool svc_wake_was_triggered(void) {
    bool val = s_wake_triggered;
    s_wake_triggered = false; /* Consume the flag */
    return val;
}

/* ========================================================================== */
/* WAKE WORD DETECTION TASK                                                   */
/* ========================================================================== */
static void wake_detect_task(void* arg) {
    /* Allocate AFE (Audio Front End) config */
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    afe_config.wakenet_init = true;
    afe_config.wakenet_model_name = "wn9_hilexin"; /* "Hi ESP" model */
    afe_config.voice_communication_init = false;
    
    s_afe_handle = &ESP_AFE_SR_HANDLE;
    s_afe_data = s_afe_handle->create_from_config(&afe_config);
    
    int chunk_size = s_afe_handle->get_feed_chunksize(s_afe_data);
    int16_t* feed_buf = heap_caps_malloc(chunk_size * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    
    ESP_LOGI(TAG, "✓ Wake word listener started (Model: %s)", afe_config.wakenet_model_name);

    while (1) {
        /* Block until the Audio Multiplexer feeds us data */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        /* Run AFE pipeline (VAD + Noise Suppression + WakeNet) */
        afe_fetch_result_t* res = s_afe_handle->fetch(s_afe_data);
        
        if (res && res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGW(TAG, "🎤 WAKE WORD DETECTED!");
            
            /* 1. Play system chime immediately */
            svc_audio_play_system_chime();
            
            /* 2. Set flag for the Voice App */
            s_wake_triggered = true;
            
            /* 3. Request Kernel to launch Voice Assistant */
            svc_sys_request_launch("voice");
            
            /* 4. Sleep WakeNet temporarily to prevent double-triggers 
                  while the app is recording the user's command */
            vTaskDelay(pdMS_TO_TICKS(3000)); 
        }
    }
}

/* ========================================================================== */
/* PUBLIC API (Called by Audio Multiplexer)                                   */
/* ========================================================================== */
void svc_wake_feed_audio(const int16_t* pcm, size_t len) {
    if (!s_afe_data) return;
    /* Feed data into AFE pipeline. This is non-blocking. */
    s_afe_handle->feed(s_afe_data, (int16_t*)pcm);
    
    /* Wake up the fetch task to process the new audio */
    if (s_wake_task) {
        xTaskNotifyGive(s_wake_task);
    }
}

void svc_wake_init(void) {
    xTaskCreatePinnedToCore(wake_detect_task, "wake_det", 4096, NULL, 
                            configMAX_PRIORITIES - 1, &s_wake_task, 1); /* Pin to Core 1 */
}

/**
 * @file svc_clock.c
 * @brief Persistent Clock/NTP Service - Runs on Core 0, never unloaded
 */
#include "kernel_api.h"
#include "svc_clock.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>

static const char* TAG = "svc_clock";
static TaskHandle_t s_clock_task = NULL;
static bool s_ntp_synced = false;

/* ========================================================================== */
/* NTP CALLBACK                                                               */
/* ========================================================================== */
static void ntp_sync_cb(struct timeval *tv) {
    s_ntp_synced = true;
    ESP_LOGI(TAG, "✓ NTP synced: %ld", tv->tv_sec);
}

/* ========================================================================== */
/* KERNEL API IMPLEMENTATIONS                                                 */
/* ========================================================================== */

/** Called by kernel during boot */
void svc_clock_init(void) {
    /* Configure SNTP */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);
    esp_sntp_init();

    /* Create low-priority monitoring task */
    xTaskCreatePinnedToCore(
        [](void*) {
            while (1) {
                if (!s_ntp_synced && esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                    s_ntp_synced = true;
                    ESP_LOGI(TAG, "NTP sync achieved");
                }
                vTaskDelay(pdMS_TO_TICKS(30000)); /* Check every 30s */
            }
        },
        "clock_svc", 2048, NULL, tskIDLE_PRIORITY + 1, &s_clock_task, 0
    );
    ESP_LOGI(TAG, "✓ Clock service initialized");
}

/* These functions are registered in g_kernel_api.sys */
uint32_t svc_clock_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

bool svc_clock_is_synced(void) {
    return s_ntp_synced;
}

k_err_t svc_clock_get_time(struct tm* out_tm) {
    if (!out_tm) return K_ERR_INVALID;
    time_t now;
    time(&now);
    localtime_r(&now, out_tm);
    return K_OK;
}

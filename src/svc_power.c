#include "svc_power.h"
#include "svc_display.h" /* For backlight control */
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "svc_power";

#define TIMEOUT_DIM_MS      30000  /* 30s */
#define TIMEOUT_SCREEN_OFF  45000  /* 45s */
#define TIMEOUT_LIGHT_SLEEP 60000  /* 60s */
#define TIMEOUT_DEEP_SLEEP  300000 /* 5 min */

static struct {
    pm_state_t current_state;
    pm_lock_t  active_locks[8]; /* Support up to 8 concurrent locks */
    int        lock_count;
    uint32_t   last_activity_ms;
    SemaphoreHandle_t mutex;
} s_pm;

static pm_lock_t get_highest_lock(void) {
    pm_lock_t highest = PM_LOCK_NONE;
    for (int i = 0; i < s_pm.lock_count; i++) {
        if (s_pm.active_locks[i] > highest) highest = s_pm.active_locks[i];
    }
    return highest;
}

void svc_power_acquire(pm_lock_t lock) {
    xSemaphoreTake(s_pm.mutex, portMAX_DELAY);
    if (s_pm.lock_count < 8) {
        s_pm.active_locks[s_pm.lock_count++] = lock;
    }
    s_pm.last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    xSemaphoreGive(s_pm.mutex);
}

void svc_power_release(pm_lock_t lock) {
    xSemaphoreTake(s_pm.mutex, portMAX_DELAY);
    for (int i = 0; i < s_pm.lock_count; i++) {
        if (s_pm.active_locks[i] == lock) {
            s_pm.active_locks[i] = s_pm.active_locks[--s_pm.lock_count];
            break;
        }
    }
    xSemaphoreGive(s_pm.mutex);
}

void svc_power_wake(void) {
    s_pm.last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_pm.current_state != PM_STATE_ACTIVE) {
        ESP_LOGI(TAG, "System Woken by event");
        svc_display_set_backlight(100);
        s_pm.current_state = PM_STATE_ACTIVE;
    }
}

static void power_monitor_task(void* arg) {
    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t idle_ms = now - s_pm.last_activity_ms;
        pm_lock_t lock = get_highest_lock();
        
        pm_state_t target_state = PM_STATE_ACTIVE;
        
        if (lock == PM_LOCK_FULL_AWAKE) {
            target_state = PM_STATE_ACTIVE;
        } else if (lock == PM_LOCK_CPU_AWAKE) {
            target_state = (idle_ms > TIMEOUT_SCREEN_OFF) ? PM_STATE_DISPLAY_OFF : PM_STATE_ACTIVE;
        } else if (lock == PM_LOCK_DISPLAY_ON) {
            target_state = PM_STATE_ACTIVE;
        } else {
            /* No locks - follow idle timers */
            if (idle_ms > TIMEOUT_DEEP_SLEEP) target_state = PM_STATE_DEEP_SLEEP;
            else if (idle_ms > TIMEOUT_LIGHT_SLEEP) target_state = PM_STATE_LIGHT_SLEEP;
            else if (idle_ms > TIMEOUT_SCREEN_OFF) target_state = PM_STATE_DISPLAY_OFF;
            else if (idle_ms > TIMEOUT_DIM_MS) target_state = PM_STATE_DIMMED;
        }
        
        if (target_state != s_pm.current_state) {
            ESP_LOGI(TAG, "State transition: %d -> %d", s_pm.current_state, target_state);
            
            switch (target_state) {
                case PM_STATE_DIMMED:
                    svc_display_set_backlight(20);
                    break;
                case PM_STATE_DISPLAY_OFF:
                    svc_display_set_backlight(0);
                    break;
                case PM_STATE_LIGHT_SLEEP:
                    svc_display_set_backlight(0);
                    /* Enter light sleep loop in Wake Service */
                    extern void svc_wake_enter_light_sleep();
                    svc_wake_enter_light_sleep(); 
                    break;
                case PM_STATE_DEEP_SLEEP:
                    ESP_LOGW(TAG, "Entering Deep Sleep. Wake via Button.");
                    esp_deep_sleep_start(); /* Never returns until hardware reset */
                    break;
                default: break;
            }
            s_pm.current_state = target_state;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void svc_power_init(void) {
    memset(&s_pm, 0, sizeof(s_pm));
    s_pm.mutex = xSemaphoreCreateMutex();
    s_pm.current_state = PM_STATE_ACTIVE;
    s_pm.last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    xTaskCreatePinnedToCore(power_monitor_task, "pwr_mon", 2048, NULL, 
                            tskIDLE_PRIORITY + 1, NULL, 0);
}

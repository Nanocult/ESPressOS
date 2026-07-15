#include "svc_battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <math.h>

static const char* TAG = "svc_battery";

/* Hardware configuration */
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_0  // GPIO 1
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define CHARGING_STATUS_GPIO    GPIO_NUM_2     // TP4056 CHRG pin
#define FULL_STATUS_GPIO        GPIO_NUM_3     // TP4056 STDBY pin (optional)

/* Voltage divider ratio (R1 + R2) / R2 */
#define VOLTAGE_DIVIDER_RATIO   2.0f

/* LiPo battery voltage curve (3.7V nominal) */
#define BATTERY_VOLTAGE_MAX     4.20f  // Fully charged
#define BATTERY_VOLTAGE_MIN     3.20f  // Empty (cutoff)
#define BATTERY_VOLTAGE_NOMINAL 3.70f

/* Monitoring intervals */
#define BATTERY_MONITOR_INTERVAL_MS  60000  // Check every 60 seconds
#define BATTERY_HISTORY_SIZE         24     // 24 hours of history

/* Battery state */
static struct {
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    bool cali_initialized;
    
    battery_info_t current_info;
    battery_event_cb_t event_callback;
    
    float voltage_history[BATTERY_HISTORY_SIZE];
    int history_index;
    int history_count;
    
    uint32_t last_update_ms;
    float discharge_rate_v_per_hour;  // Calculated discharge rate
    
    TaskHandle_t monitor_task;
    SemaphoreHandle_t mutex;
    nvs_handle_t nvs_handle;
} s_battery = {0};

/* ========================================================================== */
/* VOLTAGE TO PERCENTAGE CONVERSION                                           */
/* ========================================================================== */

static uint8_t voltage_to_percentage(float voltage) {
    if (voltage >= BATTERY_VOLTAGE_MAX) return 100;
    if (voltage <= BATTERY_VOLTAGE_MIN) return 0;
    
    // Non-linear LiPo discharge curve approximation
    // This is a simplified model - real batteries have complex curves
    float normalized = (voltage - BATTERY_VOLTAGE_MIN) / (BATTERY_VOLTAGE_MAX - BATTERY_VOLTAGE_MIN);
    
    // Apply sigmoid-like curve for more realistic percentage
    // Steep drop at beginning and end, flatter in middle
    if (normalized > 0.8f) {
        // 80-100%: Steep drop from 4.2V to 4.0V
        return 80 + (normalized - 0.8f) * 100.0f;
    } else if (normalized > 0.2f) {
        // 20-80%: Linear region
        return 20 + (normalized - 0.2f) * 100.0f;
    } else {
        // 0-20%: Steep drop from 3.4V to 3.2V
        return normalized * 100.0f;
    }
}

static battery_level_t percentage_to_level(uint8_t percentage) {
    if (percentage < 10) return BATTERY_LEVEL_CRITICAL;
    if (percentage < 20) return BATTERY_LEVEL_LOW;
    if (percentage < 50) return BATTERY_LEVEL_MEDIUM;
    if (percentage < 80) return BATTERY_LEVEL_GOOD;
    return BATTERY_LEVEL_FULL;
}

/* ========================================================================== */
/* HARDWARE READ FUNCTIONS                                                    */
/* ========================================================================== */

static float read_battery_voltage(void) {
    if (!s_battery.adc_handle) return 0.0f;
    
    int raw_value;
    esp_err_t ret = adc_oneshot_read(s_battery.adc_handle, BATTERY_ADC_CHANNEL, &raw_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return 0.0f;
    }
    
    // Convert to voltage (millivolts)
    int voltage_mv;
    if (s_battery.cali_initialized) {
        ret = adc_cali_raw_to_voltage(s_battery.cali_handle, raw_value, &voltage_mv);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC calibration failed: %s", esp_err_to_name(ret));
            return 0.0f;
        }
    } else {
        // Fallback: assume 3.3V reference, 12-bit ADC
        voltage_mv = (raw_value * 3300) / 4095;
    }
    
    // Apply voltage divider ratio
    float battery_voltage = (voltage_mv / 1000.0f) * VOLTAGE_DIVIDER_RATIO;
    
    return battery_voltage;
}

static battery_state_t read_charging_state(void) {
    // Read charging status pin (active LOW when charging)
    int chrg_level = gpio_get_level(CHARGING_STATUS_GPIO);
    
    if (chrg_level == 0) {
        return BATTERY_STATE_CHARGING;
    }
    
    // Check if battery is full (optional STDBY pin)
    if (FULL_STATUS_GPIO != GPIO_NUM_NC) {
        int stdby_level = gpio_get_level(FULL_STATUS_GPIO);
        if (stdby_level == 0) {
            return BATTERY_STATE_FULL;
        }
    }
    
    return BATTERY_STATE_DISCHARGING;
}

/* ========================================================================== */
/* DISCHARGE RATE CALCULATION                                                 */
/* ========================================================================== */

static void update_discharge_rate(float current_voltage) {
    // Calculate discharge rate based on voltage drop over time
    if (s_battery.history_count < 2) return;
    
    // Get voltage from 1 hour ago
    int prev_index = (s_battery.history_index - 1 + BATTERY_HISTORY_SIZE) % BATTERY_HISTORY_SIZE;
    float prev_voltage = s_battery.voltage_history[prev_index];
    
    if (prev_voltage > 0.0f && current_voltage < prev_voltage) {
        // Voltage dropped - calculate rate (V per hour)
        s_battery.discharge_rate_v_per_hour = prev_voltage - current_voltage;
        
        // Smooth the rate with exponential moving average
        static float smoothed_rate = 0.0f;
        if (smoothed_rate == 0.0f) {
            smoothed_rate = s_battery.discharge_rate_v_per_hour;
        } else {
            smoothed_rate = (smoothed_rate * 0.7f) + (s_battery.discharge_rate_v_per_hour * 0.3f);
        }
        s_battery.discharge_rate_v_per_hour = smoothed_rate;
    }
}

static uint32_t calculate_remaining_minutes(float current_voltage) {
    if (s_battery.discharge_rate_v_per_hour <= 0.0f) return 0;
    if (s_battery.current_info.state != BATTERY_STATE_DISCHARGING) return 0;
    
    float voltage_remaining = current_voltage - BATTERY_VOLTAGE_MIN;
    float hours_remaining = voltage_remaining / s_battery.discharge_rate_v_per_hour;
    
    return (uint32_t)(hours_remaining * 60.0f);
}

/* ========================================================================== */
/* MONITORING TASK                                                            */
/* ========================================================================== */

static void battery_monitor_task(void* arg) {
    ESP_LOGI(TAG, "Battery monitor task started");
    
    while (1) {
        xSemaphoreTake(s_battery.mutex, portMAX_DELAY);
        
        // Read current voltage
        float voltage = read_battery_voltage();
        battery_state_t state = read_charging_state();
        
        // Update battery info
        s_battery.current_info.voltage = voltage;
        s_battery.current_info.percentage = voltage_to_percentage(voltage);
        s_battery.current_info.state = state;
        s_battery.current_info.level = percentage_to_level(s_battery.current_info.percentage);
        
        // Update uptime
        s_battery.current_info.uptime_seconds = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        
        // Update voltage history (once per hour)
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - s_battery.last_update_ms >= 3600000) { // 1 hour
            s_battery.voltage_history[s_battery.history_index] = voltage;
            s_battery.history_index = (s_battery.history_index + 1) % BATTERY_HISTORY_SIZE;
            if (s_battery.history_count < BATTERY_HISTORY_SIZE) {
                s_battery.history_count++;
            }
            s_battery.last_update_ms = now_ms;
            
            // Update discharge rate
            if (state == BATTERY_STATE_DISCHARGING) {
                update_discharge_rate(voltage);
            }
        }
        
        // Calculate remaining time
        s_battery.current_info.current_ma = 0; // TODO: Read from fuel gauge if available
        uint32_t remaining_min = calculate_remaining_minutes(voltage);
        
        // Save to NVS periodically
        static int save_counter = 0;
        if (++save_counter >= 60) { // Every 60 minutes
            nvs_set_blob(s_battery.nvs_handle, "bat_hist", s_battery.voltage_history, sizeof(s_battery.voltage_history));
            nvs_set_u8(s_battery.nvs_handle, "bat_hist_idx", s_battery.history_index);
            nvs_set_u8(s_battery.nvs_handle, "bat_hist_cnt", s_battery.history_count);
            nvs_commit(s_battery.nvs_handle);
            save_counter = 0;
        }
        
        xSemaphoreGive(s_battery.mutex);
        
        // Invoke callback if registered
        if (s_battery.event_callback) {
            s_battery.event_callback(&s_battery.current_info);
        }
        
        // Log warnings
        if (s_battery.current_info.level == BATTERY_LEVEL_CRITICAL && state == BATTERY_STATE_DISCHARGING) {
            ESP_LOGW(TAG, "⚠️ Battery CRITICAL: %.2fV (%u%%)", voltage, s_battery.current_info.percentage);
        } else if (s_battery.current_info.level == BATTERY_LEVEL_LOW && state == BATTERY_STATE_DISCHARGING) {
            ESP_LOGW(TAG, "⚠️ Battery LOW: %.2fV (%u%%)", voltage, s_battery.current_info.percentage);
        }
        
        vTaskDelay(pdMS_TO_TICKS(BATTERY_MONITOR_INTERVAL_MS));
    }
}

/* ========================================================================== */
/* PUBLIC API                                                                 */
/* ========================================================================== */

k_err_t svc_battery_init(void) {
    ESP_LOGI(TAG, "Initializing battery service...");
    
    // Initialize ADC
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_battery.adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(ret));
        return K_ERR_IO;
    }
    
    // Configure ADC channel
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_11,  // Full-scale voltage ~3.9V
    };
    
    ret = adc_oneshot_config_channel(s_battery.adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return K_ERR_IO;
    }
    
    // Initialize ADC calibration
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_12,
    };
    
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_battery.cali_handle);
    if (ret == ESP_OK) {
        s_battery.cali_initialized = true;
        ESP_LOGI(TAG, "ADC calibration initialized");
    } else {
        ESP_LOGW(TAG, "ADC calibration not available");
    }
    
    // Configure charging status GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CHARGING_STATUS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    if (FULL_STATUS_GPIO != GPIO_NUM_NC) {
        io_conf.pin_bit_mask = (1ULL << FULL_STATUS_GPIO);
        gpio_config(&io_conf);
    }
    
    // Initialize NVS
    esp_err_t err = nvs_open("battery", NVS_READWRITE, &s_battery.nvs_handle);
    if (err == ESP_OK) {
        // Load history from NVS
        size_t len = sizeof(s_battery.voltage_history);
        nvs_get_blob(s_battery.nvs_handle, "bat_hist", s_battery.voltage_history, &len);
        uint8_t idx = 0, cnt = 0;
        nvs_get_u8(s_battery.nvs_handle, "bat_hist_idx", &idx);
        nvs_get_u8(s_battery.nvs_handle, "bat_hist_cnt", &cnt);
        s_battery.history_index = idx;
        s_battery.history_count = cnt;
    }
    
    // Create mutex
    s_battery.mutex = xSemaphoreCreateMutex();
    
    // Initialize state
    memset(&s_battery.current_info, 0, sizeof(battery_info_t));
    s_battery.last_update_ms = 0;
    s_battery.discharge_rate_v_per_hour = 0.0f;
    
    // Start monitor task
    xTaskCreatePinnedToCore(battery_monitor_task, "bat_mon", 4096, NULL, 3, &s_battery.monitor_task, 0);
    
    ESP_LOGI(TAG, "✓ Battery service initialized");
    return K_OK;
}

k_err_t svc_battery_get_info(battery_info_t* out_info) {
    if (!out_info) return K_ERR_INVALID;
    
    xSemaphoreTake(s_battery.mutex, portMAX_DELAY);
    *out_info = s_battery.current_info;
    xSemaphoreGive(s_battery.mutex);
    
    return K_OK;
}

uint8_t svc_battery_get_percentage(void) {
    return s_battery.current_info.percentage;
}

battery_state_t svc_battery_get_state(void) {
    return s_battery.current_info.state;
}

bool svc_battery_is_charging(void) {
    return s_battery.current_info.state == BATTERY_STATE_CHARGING;
}

bool svc_battery_is_low(void) {
    return s_battery.current_info.level == BATTERY_LEVEL_LOW || 
           s_battery.current_info.level == BATTERY_LEVEL_CRITICAL;
}

bool svc_battery_is_critical(void) {
    return s_battery.current_info.level == BATTERY_LEVEL_CRITICAL;
}

void svc_battery_register_callback(battery_event_cb_t callback) {
    s_battery.event_callback = callback;
}

k_err_t svc_battery_get_history(float* voltage_array, int* count, int max_count) {
    if (!voltage_array || !count) return K_ERR_INVALID;
    
    xSemaphoreTake(s_battery.mutex, portMAX_DELAY);
    
    int actual_count = (s_battery.history_count < max_count) ? s_battery.history_count : max_count;
    *count = actual_count;
    
    for (int i = 0; i < actual_count; i++) {
        int idx = (s_battery.history_index - actual_count + i + BATTERY_HISTORY_SIZE) % BATTERY_HISTORY_SIZE;
        voltage_array[i] = s_battery.voltage_history[idx];
    }
    
    xSemaphoreGive(s_battery.mutex);
    return K_OK;
}

uint32_t svc_battery_get_remaining_minutes(void) {
    return calculate_remaining_minutes(s_battery.current_info.voltage);
}

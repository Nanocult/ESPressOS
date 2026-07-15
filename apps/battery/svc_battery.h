#ifndef SVC_BATTERY_H
#define SVC_BATTERY_H

#include "kernel_api.h"
#include <stdbool.h>

typedef enum {
    BATTERY_STATE_DISCHARGING,
    BATTERY_STATE_CHARGING,
    BATTERY_STATE_FULL,
    BATTERY_STATE_UNKNOWN,
} battery_state_t;

typedef enum {
    BATTERY_LEVEL_CRITICAL,  // < 10%
    BATTERY_LEVEL_LOW,       // 10-20%
    BATTERY_LEVEL_MEDIUM,    // 20-50%
    BATTERY_LEVEL_GOOD,      // 50-80%
    BATTERY_LEVEL_FULL,      // > 80%
} battery_level_t;

typedef struct {
    float voltage;           // Battery voltage in volts
    uint8_t percentage;      // 0-100%
    battery_state_t state;   // Charging/discharging/full
    battery_level_t level;   // Critical/low/medium/good/full
    float current_ma;        // Current draw (if fuel gauge available)
    uint32_t uptime_seconds; // Device uptime
} battery_info_t;

typedef void (*battery_event_cb_t)(battery_info_t* info);

/**
 * Initialize battery service
 */
k_err_t svc_battery_init(void);

/**
 * Get current battery information
 */
k_err_t svc_battery_get_info(battery_info_t* out_info);

/**
 * Get battery percentage (0-100)
 */
uint8_t svc_battery_get_percentage(void);

/**
 * Get battery state
 */
battery_state_t svc_battery_get_state(void);

/**
 * Check if battery is charging
 */
bool svc_battery_is_charging(void);

/**
 * Check if battery is low (< 20%)
 */
bool svc_battery_is_low(void);

/**
 * Check if battery is critical (< 10%)
 */
bool svc_battery_is_critical(void);

/**
 * Register callback for battery events (percentage change, state change)
 */
void svc_battery_register_callback(battery_event_cb_t callback);

/**
 * Get battery voltage history (last 24 hours, 1 sample per hour)
 */
k_err_t svc_battery_get_history(float* voltage_array, int* count, int max_count);

/**
 * Get estimated remaining time in minutes (based on average discharge rate)
 */
uint32_t svc_battery_get_remaining_minutes(void);

#endif

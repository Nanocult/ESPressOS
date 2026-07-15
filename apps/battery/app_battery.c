/**
 * @file app_battery.c
 * @brief ESP-AppOS Battery Management App
 */
#include "kernel_api.h"
#include <stdio.h>
#include <string.h>

static const KernelAPI* g_api;

/* UI Elements */
static k_lvgl_obj_t g_lbl_voltage;
static k_lvgl_obj_t g_lbl_percentage;
static k_lvgl_obj_t g_lbl_state;
static k_lvgl_obj_t g_lbl_remaining;
static k_lvgl_obj_t g_chart_history;
static k_lvgl_obj_t g_bar_percentage;

/* ========================================================================== */
/* UI UPDATE                                                                  */
/* ========================================================================== */

static void update_battery_ui(void* arg) {
    battery_info_t info;
    if (g_api->battery.get_info(&info) != K_OK) return;
    
    // Update voltage label
    char voltage_str[32];
    snprintf(voltage_str, sizeof(voltage_str), "%.2fV", info.voltage);
    g_api->display.obj_set_prop(g_lbl_voltage, "text", voltage_str);
    
    // Update percentage label
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%u%%", info.percentage);
    g_api->display.obj_set_prop(g_lbl_percentage, "text", pct_str);
    
    // Update progress bar
    char pct_val[8];
    snprintf(pct_val, sizeof(pct_val), "%u", info.percentage);
    g_api->display.obj_set_prop(g_bar_percentage, "value", pct_val);
    
    // Update state label
    const char* state_str;
    switch (info.state) {
        case BATTERY_STATE_CHARGING: state_str = "Charging"; break;
        case BATTERY_STATE_FULL: state_str = "Full"; break;
        case BATTERY_STATE_DISCHARGING: state_str = "Discharging"; break;
        default: state_str = "Unknown"; break;
    }
    g_api->display.obj_set_prop(g_lbl_state, "text", state_str);
    
    // Update remaining time
    if (info.state == BATTERY_STATE_DISCHARGING) {
        uint32_t remaining_min = g_api->battery.get_remaining_minutes();
        if (remaining_min > 0) {
            char remaining_str[32];
            if (remaining_min >= 60) {
                snprintf(remaining_str, sizeof(remaining_str), "%uh %um remaining", 
                         remaining_min / 60, remaining_min % 60);
            } else {
                snprintf(remaining_str, sizeof(remaining_str), "%um remaining", remaining_min);
            }
            g_api->display.obj_set_prop(g_lbl_remaining, "text", remaining_str);
        } else {
            g_api->display.obj_set_prop(g_lbl_remaining, "text", "Calculating...");
        }
    } else if (info.state == BATTERY_STATE_CHARGING) {
        g_api->display.obj_set_prop(g_lbl_remaining, "text", "Charging...");
    } else {
        g_api->display.obj_set_prop(g_lbl_remaining, "text", "");
    }
    
    // Update chart with history
    float history[24];
    int count = 0;
    if (g_api->battery.get_history(history, &count, 24) == K_OK && count > 0) {
        // Add new data point to chart
        lv_chart_set_next_value(g_chart_history, lv_chart_get_series_next(g_chart_history, NULL), 
                                (int)(info.voltage * 100)); // Scale to 0-500 range
    }
    
    // Color code based on level
    const char* color;
    switch (info.level) {
        case BATTERY_LEVEL_CRITICAL: color = "0xFF0000"; break; // Red
        case BATTERY_LEVEL_LOW: color = "0xFFA500"; break;     // Orange
        case BATTERY_LEVEL_MEDIUM: color = "0xFFFF00"; break;  // Yellow
        case BATTERY_LEVEL_GOOD: color = "0x00FF00"; break;    // Green
        case BATTERY_LEVEL_FULL: color = "0x00FF00"; break;    // Green
        default: color = "0xFFFFFF"; break;
    }
    g_api->display.obj_set_prop(g_lbl_percentage, "text_color", color);
}

/* ========================================================================== */
/* APP ENTRY                                                                  */
/* ========================================================================== */

void app_main(const KernelAPI* api) {
    g_api = api;
    
    if (api->abi_version < KERNEL_ABI_VERSION) {
        api->sys.request_exit();
        return;
    }
    
    api->display.init_screen(240, 320);
    
    // Header
    k_lvgl_obj_t header = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(header, "text", "Battery Manager");
    api->display.obj_set_prop(header, "align", "top_mid");
    api->display.obj_set_prop(header, "y", "10");
    api->display.obj_set_prop(header, "text_font", "montserrat_24");
    
    // Percentage display (large)
    g_lbl_percentage = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_percentage, "text", "0%");
    api->display.obj_set_prop(g_lbl_percentage, "align", "top_mid");
    api->display.obj_set_prop(g_lbl_percentage, "y", "50");
    api->display.obj_set_prop(g_lbl_percentage, "text_font", "montserrat_48");
    
    // Progress bar
    g_bar_percentage = api->display.obj_create("bar", NULL);
    api->display.obj_set_prop(g_bar_percentage, "width", "200");
    api->display.obj_set_prop(g_bar_percentage, "height", "20");
    api->display.obj_set_prop(g_bar_percentage, "align", "top_mid");
    api->display.obj_set_prop(g_bar_percentage, "y", "110");
    api->display.obj_set_prop(g_bar_percentage, "range", "0 100");
    api->display.obj_set_prop(g_bar_percentage, "value", "0");
    
    // Voltage label
    g_lbl_voltage = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_voltage, "text", "0.00V");
    api->display.obj_set_prop(g_lbl_voltage, "align", "top_left");
    api->display.obj_set_prop(g_lbl_voltage, "x", "20");
    api->display.obj_set_prop(g_lbl_voltage, "y", "140");
    api->display.obj_set_prop(g_lbl_voltage, "text_font", "montserrat_16");
    
    // State label
    g_lbl_state = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_state, "text", "Unknown");
    api->display.obj_set_prop(g_lbl_state, "align", "top_right");
    api->display.obj_set_prop(g_lbl_state, "x", "-20");
    api->display.obj_set_prop(g_lbl_state, "y", "140");
    api->display.obj_set_prop(g_lbl_state, "text_font", "montserrat_16");
    
    // Remaining time label
    g_lbl_remaining = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_remaining, "text", "Calculating...");
    api->display.obj_set_prop(g_lbl_remaining, "align", "top_mid");
    api->display.obj_set_prop(g_lbl_remaining, "y", "170");
    api->display.obj_set_prop(g_lbl_remaining, "text_font", "montserrat_14");
    
    // Voltage history chart
    g_chart_history = api->display.obj_create("chart", NULL);
    api->display.obj_set_prop(g_chart_history, "width", "220");
    api->display.obj_set_prop(g_chart_history, "height", "100");
    api->display.obj_set_prop(g_chart_history, "align", "bottom_mid");
    api->display.obj_set_prop(g_chart_history, "y", "-20");
    api->display.obj_set_prop(g_chart_history, "type", "line");
    api->display.obj_set_prop(g_chart_history, "point_count", "24");
    api->display.obj_set_prop(g_chart_history, "range", "300 450"); // 3.00V to 4.50V
    
    // Start periodic UI update timer
    k_timer_t ui_timer;
    api->sys.timer_create(1000, true, update_battery_ui, NULL, &ui_timer);
    
    // Initial update
    update_battery_ui(NULL);
    
    api->display.flush();
    api->sys.log(2, "Battery", "App initialized");
}

void app_deinit(void) {
    // Cleanup handled by kernel
}

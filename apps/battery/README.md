# Battery Management App

Battery App is a complete battery monitoring system with hardware integration, a dedicated service, and a full-featured Battery Management App with hardware monitoring, percentage calculation, charging detection, history tracking, and automatic low-battery protection!

## Hardware Requirements

### Basic Setup (Voltage Divider + Charging IC)
```
Battery (+) ──┬── R1 (100kΩ) ──┬── R2 (100kΩ) ── GND
              │                 │
              └─────────────────┴── GPIO 1 (ADC1_CH0)
              
TP4056 CHRG pin ── GPIO 2 (Active LOW when charging)
TP4056 STDBY pin ── GPIO 3 (Active LOW when full)
```

### Advanced Setup (Fuel Gauge IC - Optional)
```
MAX17048 I2C:
- SDA → GPIO 8
- SCL → GPIO 9
```

## Kernel API Extension

Update `kernel_api.h` (ABI v1.8):

```c
typedef struct {
    k_err_t (*init)(void);
    k_err_t (*get_info)(void* out_info);  // battery_info_t*
    uint8_t (*get_percentage)(void);
    int (*get_state)(void);  // battery_state_t
    bool (*is_charging)(void);
    bool (*is_low)(void);
    bool (*is_critical)(void);
    uint32_t (*get_remaining_minutes)(void);
    k_err_t (*get_history)(float* voltage_array, int* count, int max_count);
} k_battery_api_t;

// Add to KernelAPI struct:
typedef struct {
    // ... existing fields ...
    k_battery_api_t battery;
} KernelAPI;
```

Wire in `kernel_main.c`:

```c
.battery = {
    .init = svc_battery_init,
    .get_info = svc_battery_get_info,
    .get_percentage = svc_battery_get_percentage,
    .get_state = (int (*)(void))svc_battery_get_state,
    .is_charging = svc_battery_is_charging,
    .is_low = svc_battery_is_low,
    .is_critical = svc_battery_is_critical,
    .get_remaining_minutes = svc_battery_get_remaining_minutes,
    .get_history = svc_battery_get_history,
},
```

---

## Low Battery Warning Integration

Add automatic low battery warnings to the kernel lifecycle:

```c
// In kernel_main.c, add to power_monitor_task or create new task:
static void battery_warning_task(void* arg) {
    bool warning_shown = false;
    bool critical_shown = false;
    
    while (1) {
        if (svc_battery_is_critical() && !critical_shown) {
            ESP_LOGE("KERNEL", "🪫 BATTERY CRITICAL! Shutting down in 60 seconds...");
            // Show overlay warning
            svc_display_show_system_overlay("BATTERY CRITICAL\nShutting down...");
            
            // Auto-shutdown after 60 seconds if still critical
            vTaskDelay(pdMS_TO_TICKS(60000));
            if (svc_battery_is_critical() && !svc_battery_is_charging()) {
                ESP_LOGW("KERNEL", "Entering deep sleep to protect battery");
                esp_deep_sleep_start();
            }
            critical_shown = true;
        } else if (svc_battery_is_low() && !warning_shown) {
            ESP_LOGW("KERNEL", "⚠️ Battery low: %u%%", svc_battery_get_percentage());
            svc_display_show_system_overlay("Battery Low\nPlease charge");
            vTaskDelay(pdMS_TO_TICKS(5000)); // Show for 5 seconds
            svc_display_dismiss_overlay();
            warning_shown = true;
        } else if (!svc_battery_is_low()) {
            warning_shown = false;
            critical_shown = false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
    }
}
```

---

## Build & Deploy

```bash
# Rebuild kernel with battery service
cd esp-appos
idf.py build flash monitor

# Build battery app
cd apps/battery
mkdir build && cd build
cmake .. && make build_espapp

# Deploy to SD card
cp battery.espapp /mnt/sd/apps/
echo '{"name":"Battery","build":1}' > /mnt/sd/apps/battery.manifest.json
```

---

## Validation Matrix

| Test | Expected Result |
|:-----|:----------------|
| Battery voltage reading | Accurate within ±0.05V of multimeter |
| Percentage calculation | Matches voltage curve (100% at 4.2V, 0% at 3.2V) |
| Charging detection | State changes to "Charging" when USB plugged in |
| Low battery warning | Overlay appears at <20%, auto-shutdown at <10% |
| History chart | Shows 24-hour voltage trend |
| Remaining time | Accurate within ±30 minutes after 2+ hours of data |
| NVS persistence | History survives reboot |
| Memory usage | <5KB RAM, no leaks after 24h operation |

## Advanced Features (Future)

1. **Fuel gauge IC support** (MAX17048) for accurate percentage
2. **Battery health monitoring** (cycle count, capacity degradation)
3. **Charging profile visualization** (voltage/current over time)
4. **Power consumption breakdown** by app/service
5. **Battery calibration wizard**
6. **Temperature monitoring** (if thermistor available)

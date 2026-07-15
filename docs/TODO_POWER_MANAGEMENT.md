# Power Management Integration

**Light Sleep Integration + VAD (Voice Activity Detection)** to drop the idle power consumption from ~120mA down to ~15mA while keeping the "Hey ESP" wake word active.

This is the final architectural pillar of a consumer-grade embedded OS. Without power management, a desktop assistant will run hot and waste energy. With it, the device becomes an "always-on" ambient appliance that sips power until spoken to.

On the ESP32-S3, achieving ~15mA idle current requires a technique called **VAD-Gated Light Sleep**. The CPU cannot run the heavy WakeNet neural network while asleep. Instead, we wake the CPU for ~5ms every 30ms to read the microphone, run the ultra-lightweight Voice Activity Detector (VAD), and if no speech is detected, immediately put the CPU back to Light Sleep.

### 1. Power Manager Service (`svc_power.h`)

This service tracks "Power Locks" requested by apps. An app playing audio needs the CPU awake but allows the display to turn off. The Voice Assistant needs everything fully awake.

### 2. Power Manager Implementation (`svc_power.c`)

This task runs at the lowest priority. It monitors idle timers and transitions the hardware into Light Sleep when safe.

### 3. VAD-Gated Light Sleep (`svc_wake.c`)

We refactor the wake word task to use ESP-IDF's Light Sleep API. When the system is in `PM_STATE_LIGHT_SLEEP`, this task loops: Wake -> Read I2S -> Run VAD -> Sleep.

```c
/* Add to svc_wake.c */
#include "esp_sleep.h"
#include "svc_power.h"

static volatile bool s_light_sleep_enabled = false;

void svc_wake_enter_light_sleep(void) {
    s_light_sleep_enabled = true;
}

/* Modify the existing wake_detect_task */
static void wake_detect_task(void* arg) {
    // ... [AFE initialization] ...
    
    int chunk_size = s_afe_handle->get_feed_chunksize(s_afe_data);
    int16_t* feed_buf = heap_caps_malloc(chunk_size * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    
    while (1) {
        /* 1. Read Audio (CPU is awake) */
        size_t bytes_read = 0;
        i2s_channel_read(s_rx_handle, feed_buf, chunk_size * sizeof(int16_t), &bytes_read, portMAX_DELAY);
        
        /* 2. Feed AFE & Fetch Result */
        s_afe_handle->feed(s_afe_data, feed_buf);
        afe_fetch_result_t* res = s_afe_handle->fetch(s_afe_data);
        
        if (!res) continue;

        /* 3. Check for Wake Word */
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGW(TAG, "🎤 WAKE WORD DETECTED!");
            s_light_sleep_enabled = false; /* Disable sleep while app runs */
            svc_power_wake();              /* Turn on display */
            svc_audio_play_system_chime(); 
            s_wake_triggered = true;
            svc_sys_request_launch("voice");
            continue;
        }

        /* 4. VAD-Gated Light Sleep Logic */
        if (s_light_sleep_enabled && res->vad_state == VAD_SILENCE) {
            /* No speech detected. Go back to sleep for ~25ms.
             * We use a timer wake to ensure we wake up for the next audio chunk. */
            esp_sleep_enable_timer_wakeup(20 * 1000); /* 20ms */
            
            /* esp_light_sleep_start() blocks until the timer fires.
             * During this time, CPU cores are halted, APB clock drops. 
             * I2S DMA continues filling the buffer in hardware. */
            esp_light_sleep_start(); 
        }
    }
}
```

### 4. Kernel API Extensions for Apps

Apps must be able to tell the kernel they are doing something important. Update `kernel_api.h` (Bump ABI to v1.6):

```c
/* Add to k_sys_api_t (append only): */
typedef struct {
    // ... existing fields ...
    
    /** Acquire power lock. Prevents system from sleeping. */
    void (*power_acquire)(int lock_type);
    
    /** Release power lock. */
    void (*power_release)(int lock_type);
    
    /** Notify system of user activity (resets idle timers). */
    void (*power_ping)(void);
} k_sys_api_t;

/* Define lock types matching pm_lock_t */
#define K_PM_LOCK_DISPLAY 1
#define K_PM_LOCK_CPU     2
#define K_PM_LOCK_FULL    3
```

**Wire it in `kernel_main.c`:**
```c
.sys = {
    // ...
    .power_acquire = (void*)svc_power_acquire,
    .power_release = (void*)svc_power_release,
    .power_ping    = (void*)svc_power_wake,
}
```

### 5. App Integration Examples

**Audio Player App (`app_audio.c`):**
```c
/* When Play is pressed: */
g_api->sys.power_acquire(K_PM_LOCK_CPU); /* Keeps CPU awake for decoding */

/* When Stop/Pause is pressed: */
g_api->sys.power_release(K_PM_LOCK_CPU); /* Allows light sleep */
```

**Voice Assistant App (`app_voice.c`):**
```c
/* When recording starts: */
g_api->sys.power_acquire(K_PM_LOCK_FULL); /* Keeps display on, CPU awake */

/* When recording stops: */
g_api->sys.power_release(K_PM_LOCK_FULL);
```

**Input Manager (`svc_input.c`):**
Every time a button is pressed or the screen is touched, the Input Manager must call `svc_power_wake()` to reset the idle timers and turn the display back on.

### 6. Critical ESP-IDF `menuconfig` Settings

Light sleep on ESP32-S3 with Octal PSRAM and I2S requires specific SDK configurations. Add these to your `sdkconfig.defaults`:

```ini
# Enable Light Sleep
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_ENABLE=y

# Keep PSRAM powered during light sleep (Crucial! Otherwise app data is lost)
CONFIG_SPIRAM_ALLOW_BSS_SEG=y
# Note: Octal PSRAM cannot be fully powered down in light sleep without losing state.
# It will consume ~2-3mA, which is acceptable for the 15mA target.

# I2S DMA must be able to wake the CPU (or run continuously if APB is locked)
# For VAD-gated sleep, we rely on the Timer Wake, so I2S DMA pauses during sleep.
# The 20ms gap in audio is acceptable for VAD, but WakeNet handles the discontinuity.
```

### 7. Power Consumption Validation Matrix

Measure the current draw on the VCC rail using a multimeter or current probe:

| System State | Display | CPU | Expected Current | Validated By |
| :--- | :--- | :--- | :--- | :--- |
| **Active (Idle)** | ON (100%) | Awake | ~110 - 130 mA | User looking at clock |
| **Dimmed** | ON (20%) | Awake | ~60 - 70 mA | 30s idle timeout |
| **Display Off** | OFF | Awake | ~40 - 50 mA | Audio playing in background |
| **Light Sleep** | OFF | Sleep/Wake | **12 - 18 mA** | 60s idle. VAD gating active. |
| **Deep Sleep** | OFF | Halted | **< 100 µA** | 5 min idle. System hibernates. |

*   **Analytics:** Implement a background telemetry service that uploads crash reports and battery stats to your server when WiFi connects.
*   **Advanced AI:** Move from simple Wake Word to offline Voice Command recognition (e.g., "Turn on the light", "Set timer") using ESP-SR's `MultiNet` model, entirely on-device without needing the server.

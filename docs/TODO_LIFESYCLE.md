# Crash recovery system

NVS-stored last-good-app, auto-fallback on fault. In the embedded world, a "bootloop" (where a device continuously crashes and reboots on startup) is a fatal failure that requires physical recovery (JTAG/UART). 

To prevent this, we will implement a **Dual-Layer Crash Recovery System**. 

1. **Layer 1: Soft Crash Protection (RAM-based):** Catches apps that load but immediately fault (e.g., null pointer, divide-by-zero) and cause the sandbox to abort them.
2. **Layer 2: Hard Crash Protection (NVS-based):** Catches catastrophic system failures (Kernel panics, Task Watchdog resets, power loss during OTA) that cause the physical chip to reboot.

---

### 1. Layer 1: Soft Crash Protection (Lifecycle Manager)

If an app is corrupted or has a deterministic bug, the sandbox will catch the fault and return `LOAD_ERR_RELOC`. If this happens repeatedly for the *same* app, the Lifecycle Manager must intervene, delete the broken file, and trigger a re-download.

Update your `kernel_lifecycle_loop()` in `lifecycle_manager.c`:

```c
/* Add to lifecycle_manager.c */
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* For remove() */

static char s_last_failed_app[32] = {0};
static int  s_soft_crash_count = 0;
#define MAX_SOFT_CRASHES 3

void kernel_lifecycle_loop(void) {
    char current_app[32] = "launcher"; 
    
    while (1) {
        app_context_t* ctx = NULL;
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/apps/%s.espapp", current_app);
        
        /* Notify Recovery System we are about to load */
        svc_recovery_mark_launch(current_app);
        
        load_result_t res = app_loader_load(path, &ctx);
        
        if (res == LOAD_OK) {
            /* App loaded successfully. Run it. */
            load_result_t run_res = app_sandbox_run(ctx); 
            app_loader_unload(ctx);
            
            /* Notify Recovery System we exited cleanly */
            svc_recovery_mark_exit();
            
            if (run_res != LOAD_OK) {
                res = run_res; /* Treat sandbox crash as load failure for tracking */
            }
        }
        
        /* --- CRASH TRACKING LOGIC --- */
        if (res != LOAD_OK) {
            if (strcmp(current_app, s_last_failed_app) == 0) {
                s_soft_crash_count++;
            } else {
                strncpy(s_last_failed_app, current_app, sizeof(s_last_failed_app)-1);
                s_soft_crash_count = 1;
            }
            
            ESP_LOGE("LIFECYCLE", "App '%s' failed (%d). Strike %d/%d", 
                     current_app, res, s_soft_crash_count, MAX_SOFT_CRASHES);
            
            if (s_soft_crash_count >= MAX_SOFT_CRASHES) {
                ESP_LOGW("LIFECYCLE", "⚠️ App '%s' blacklisted. Deleting and re-provisioning.", current_app);
                remove(path); /* Delete the broken .espapp file */
                
                /* Trigger storage manager to re-download missing core apps */
                svc_storage_start_provisioning("https://ota.yourserver.com"); 
                
                s_soft_crash_count = 0; /* Reset counter */
                strcpy(current_app, "launcher"); /* Fallback to launcher */
                vTaskDelay(pdMS_TO_TICKS(2000)); /* Wait for provisioning */
                continue;
            }
        } else {
            /* Success! Reset crash counter */
            s_soft_crash_count = 0;
            s_last_failed_app[0] = '\0';
        }
        
        /* Determine next app */
        if (g_pending_launch[0] != '\0') {
            strncpy(current_app, g_pending_launch, sizeof(current_app));
            g_pending_launch[0] = '\0';
        } else {
            strcpy(current_app, "launcher");
        }
    }
}
```

---

### 2. Layer 2: Hard Crash Protection (`svc_recovery.c`)

This service uses Non-Volatile Storage (NVS) to track the boot state. If the device reboots unexpectedly while an app is marked as "LOADING", it means the kernel itself panicked or the hardware watchdog fired.

#### `svc_recovery.h`
```c
#ifndef SVC_RECOVERY_H
#define SVC_RECOVERY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RECOVERY_STATE_IDLE = 0,
    RECOVERY_STATE_LOADING,
    RECOVERY_STATE_SAFE_MODE
} recovery_state_t;

void svc_recovery_init(void);
void svc_recovery_mark_launch(const char* app_name);
void svc_recovery_mark_stable(void);
void svc_recovery_mark_exit(void);
recovery_state_t svc_recovery_get_state(void);
const char* svc_recovery_get_blacklisted_app(void);

#endif
```

#### `svc_recovery.c`
```c
#include "svc_recovery.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const char* TAG = "svc_recovery";
static nvs_handle_t s_nvs_handle;
static recovery_state_t s_current_state = RECOVERY_STATE_IDLE;
static char s_blacklisted_app[32] = {0};

#define MAX_HARD_CRASHES 3

void svc_recovery_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nvs_open("espappos", NVS_READWRITE, &s_nvs_handle));

    uint8_t state = RECOVERY_STATE_IDLE;
    nvs_get_u8(s_nvs_handle, "state", &state);

    if (state == RECOVERY_STATE_LOADING) {
        /* We rebooted while trying to load an app! Hardware crash detected. */
        char pending[32] = {0};
        size_t len = sizeof(pending);
        nvs_get_str(s_nvs_handle, "pending", pending, &len);
        
        uint8_t cnt = 0;
        nvs_get_u8(s_nvs_handle, "hard_crash", &cnt);
        cnt++;
        
        ESP_LOGW(TAG, "⚠️ Hard crash detected for '%s' (Count: %d)", pending, cnt);
        
        if (cnt >= MAX_HARD_CRASHES) {
            ESP_LOGE(TAG, "🛑 FATAL: App '%s' causing kernel panics. Entering SAFE MODE.", pending);
            strncpy(s_blacklisted_app, pending, sizeof(s_blacklisted_app)-1);
            s_current_state = RECOVERY_STATE_SAFE_MODE;
            nvs_set_u8(s_nvs_handle, "state", RECOVERY_STATE_SAFE_MODE);
            nvs_set_u8(s_nvs_handle, "hard_crash", 0); /* Reset for next time */
            
            /* Delete the broken app to prevent immediate re-crash if we exit safe mode */
            char path[64];
            snprintf(path, sizeof(path), "/sdcard/apps/%s.espapp", pending);
            remove(path);
        } else {
            nvs_set_u8(s_nvs_handle, "hard_crash", cnt);
            s_current_state = RECOVERY_STATE_IDLE;
            nvs_set_u8(s_nvs_handle, "state", RECOVERY_STATE_IDLE);
        }
    } else {
        s_current_state = RECOVERY_STATE_IDLE;
        nvs_set_u8(s_nvs_handle, "state", RECOVERY_STATE_IDLE);
        nvs_set_u8(s_nvs_handle, "hard_crash", 0);
    }
    
    nvs_commit(s_nvs_handle);
}

void svc_recovery_mark_launch(const char* app_name) {
    nvs_set_str(s_nvs_handle, "pending", app_name);
    nvs_set_u8(s_nvs_handle, "state", RECOVERY_STATE_LOADING);
    nvs_commit(s_nvs_handle);
}

void svc_recovery_mark_stable(void) {
    /* App has run for 10 seconds without crashing. It is stable. */
    nvs_set_u8(s_nvs_handle, "state", RECOVERY_STATE_IDLE);
    nvs_set_u8(s_nvs_handle, "hard_crash", 0);
    nvs_commit(s_nvs_handle);
}

void svc_recovery_mark_exit(void) {
    nvs_set_u8(s_nvs_handle, "state", RECOVERY_STATE_IDLE);
    nvs_commit(s_nvs_handle);
}

recovery_state_t svc_recovery_get_state(void) { return s_current_state; }
const char* svc_recovery_get_blacklisted_app(void) { return s_blacklisted_app; }
```

---

### 3. The 10-Second Stability Timer

We must tell the recovery system when an app has proven it won't crash the kernel. We add a 10-second timer to the `app_sandbox.c`.

**Update `app_sandbox_run()` in `app_sandbox.c`:**
```c
/* Add to app_sandbox.c */
#include "esp_timer.h"
#include "svc_recovery.h"

static void stability_timer_cb(void* arg) {
    svc_recovery_mark_stable();
    esp_timer_handle_t timer = (esp_timer_handle_t)arg;
    esp_timer_delete(timer);
}

load_result_t app_sandbox_run(app_context_t* ctx) {
    // ... existing setup ...
    
    /* Start 10s stability timer */
    esp_timer_handle_t stab_timer;
    esp_timer_create_args_t cfg = { .callback = stability_timer_cb, .arg = &stab_timer };
    esp_timer_create(&cfg, &stab_timer);
    esp_timer_start_once(stab_timer, 10 * 1000000); /* 10 seconds */

    /* Execute app */
    entry(api);

    /* Clean up timer if app exited before 10s */
    esp_timer_stop(stab_timer);
    esp_timer_delete(stab_timer);
    
    // ... existing teardown ...
}
```

---

### 4. The Kernel Safe Mode UI

If `svc_recovery_get_state() == RECOVERY_STATE_SAFE_MODE`, the kernel **must not** attempt to load the launcher. Instead, it renders a hardcoded fallback UI directly using LVGL (since the kernel links against LVGL for the display service).

**Add to `kernel_main.c`:**

```c
#include "svc_recovery.h"
#include "lvgl.h"

static void render_safe_mode_ui(const char* broken_app) {
    ESP_LOGW("KERNEL", "Rendering Safe Mode UI...");
    
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x220000), 0); /* Dark Red Background */
    
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "SYSTEM ERROR");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "App '%s' caused\nrepeated system crashes\nand was removed.", broken_app);
    
    lv_obj_t* desc = lv_label_create(scr);
    lv_label_set_text(desc, msg);
    lv_obj_set_style_text_color(desc, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(desc, 200);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, -20);
    
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, "Attempting to re-download\nfrom server...");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
    
    lv_refr_now(NULL);
    
    /* Trigger automatic re-provisioning */
    svc_storage_start_provisioning("https://ota.yourserver.com");
}

/* In app_main() of kernel_main.c: */
void app_main(void) {
    // ... init PSRAM, Clock, Audio, Display ...
    
    svc_recovery_init(); /* MUST be called before storage/lifecycle */
    svc_storage_init();
    
    if (svc_recovery_get_state() == RECOVERY_STATE_SAFE_MODE) {
        render_safe_mode_ui(svc_recovery_get_blacklisted_app());
        
        /* Wait for provisioning to finish, then reboot to clear safe mode state */
        while(svc_storage_get_state() != STORAGE_STATE_READY) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        esp_restart();
    }
    
    /* Normal boot path */
    kernel_lifecycle_loop();
}
```

---

### 5. Validation Matrix

Execute these tests to verify your device is now unbrickable:

| Test Scenario | Action | Expected Result |
| :--- | :--- | :--- |
| **Corrupt File (Soft Crash)** | Overwrite `clock.espapp` with random garbage bytes. | Loader fails signature check -> Lifecycle counts 1. Tap again -> counts 2. Tap again -> counts 3 -> **File deleted, re-downloaded from server.** |
| **Deterministic Bug (Soft Crash)** | Add `int x = 1/0;` to `app_main` in Clock app. | Sandbox catches exception -> Lifecycle counts 1...3 -> **File deleted, re-downloaded.** |
| **Kernel Panic (Hard Crash)** | Add `*(int*)0 = 0;` inside `svc_display_flush()` (Kernel space). | Device panics and reboots. NVS increments `hard_crash`. After 3 reboots -> **Enters Safe Mode UI, deletes Display Manager (if it were an app), or halts.** |
| **Power Loss during OTA** | Unplug power exactly when `http_stream` is writing to `/cache/`. | On boot, NVS state is clean (OTA doesn't set `LOADING` state). Partial `.tmp` file is ignored. **Device boots normally.** |
| **Safe Mode Recovery** | Force Safe Mode. Connect to WiFi. | Safe Mode UI renders -> Storage Manager downloads fresh apps -> Device auto-reboots -> **Boots normally into Launcher.** |

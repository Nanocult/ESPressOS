# Core Applications

## ABI Refinements (Update kernel_api.h)

Building the first real .espapp binary will expose any remaining friction in our ABI, toolchain, or kernel services. Before writing the app code, we must make two critical ABI refinements. Apps compiled with -nostdlib cannot safely include ESP-IDF headers or standard <time.h>. We must provide pure C, self-contained types and formatting helpers.

Add these definitions to kernel_api.h. This decouples apps from the kernel's internal libc/newlib versions.

```c
/* --- ADD TO kernel_api.h --- */

/** Self-contained datetime struct (replaces struct tm) */
typedef struct {
    uint16_t year;    /* e.g., 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
    uint8_t  hour;    /* 0-23 */
    uint8_t  min;     /* 0-59 */
    uint8_t  sec;     /* 0-59 */
    uint8_t  wday;    /* 0-6 (0=Sunday) */
    bool     is_synced;
} k_datetime_t;

/** Add to k_sys_api_t (append only!): */
typedef struct {
    // ... existing fields ...
    
    /** Safe integer to string conversion (base 10) */
    void (*itoa)(int val, char* buf, int min_width);
} k_sys_api_t;

/** NEW: Clock API Sub-table */
typedef struct {
    k_err_t (*get_datetime)(k_datetime_t* out_dt);
    bool (*is_synced)(void);
} k_clock_api_t;

/** Add to master KernelAPI struct (append only!): */
typedef struct {
    // ... existing fields ...
    k_clock_api_t clock;
} KernelAPI;
```

> ⚠️ Action Required: Bump KERNEL_ABI_VERSION_MINOR to 1 in kernel_api.h and build_espapp.py. Update kernel_main.c to wire svc_clock_get_datetime and a simple k_itoa implementation into g_kernel_api.

## The Clock/Calendar App (app_clock.c)

This app is entirely self-contained. It uses zero ESP-IDF headers, zero standard library functions (avoiding ABI drift), and relies exclusively on the KernelAPI.

## App Build System (CMakeLists.txt)

Create a standalone build environment for the app. This ensures it is compiled as a raw binary, not a full ESP-IDF firmware.

To build: 
```bash
cd apps/clock && mkdir build && cd build && cmake .. && make build_espapp
```

---

## Kernel-Side Implementation Updates

To make the app's string-based UI commands work, svc_display.c needs a property parser. Add this to svc_display.c:

```c
/* Add to svc_display.c */
#include <stdlib.h> /* For atoi */

k_err_t svc_display_obj_set_prop(k_lvgl_obj_t obj, const char* prop, const char* value) {
    lv_obj_t* o = (lv_obj_t*)obj;
    if (!o || !prop || !value) return K_ERR_INVALID;

    if (strcmp(prop, "text") == 0) {
        lv_label_set_text(o, value);
    } 
    else if (strcmp(prop, "align") == 0) {
        if (strcmp(value, "top_mid") == 0) lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 0);
        else if (strcmp(value, "center") == 0) lv_obj_center(o);
        else if (strcmp(value, "bottom_mid") == 0) lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    else if (strcmp(prop, "x") == 0) {
        lv_obj_set_x(o, atoi(value));
    }
    else if (strcmp(prop, "y") == 0) {
        lv_obj_set_y(o, atoi(value));
    }
    else if (strcmp(prop, "width") == 0) {
        lv_obj_set_width(o, atoi(value));
    }
    else if (strcmp(prop, "height") == 0) {
        lv_obj_set_height(o, atoi(value));
    }
    else if (strcmp(prop, "text_font") == 0) {
        /* Map string names to compiled LVGL font pointers */
        if (strcmp(value, "montserrat_48") == 0) lv_obj_set_style_text_font(o, &lv_font_montserrat_48, 0);
        else if (strcmp(value, "montserrat_24") == 0) lv_obj_set_style_text_font(o, &lv_font_montserrat_24, 0);
    }
    else if (strcmp(prop, "text_color") == 0) {
        uint32_t color = strtoul(value + 2, NULL, 16); /* Parse "0xRRGGBB" */
        lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    }
    else if (strcmp(prop, "today") == 0 && lv_obj_check_type(o, &lv_calendar_class)) {
        /* Parse "YYYY-MM-DD" */
        int y, m, d;
        if (sscanf(value, "%d-%d-%d", &y, &m, &d) == 3) {
            lv_calendar_set_today_date(o, y, m, d);
        }
    }
    else {
        return K_ERR_INVALID;
    }
    return K_OK;
}
```

Also, ensure svc_display_obj_create supports the "calendar" type:

```c
// In svc_display_obj_create:
else if (strcmp(type, "calendar") == 0) obj = lv_calendar_create(par);
```

---

### End-to-End Validation Plan

Execute this sequence to validate the entire Phase 1 + Phase 2 stack:

1.  **Compile Kernel:** Build the main ESP-IDF project with the updated `kernel_api.h`, `svc_display.c` parser, and `svc_clock.c` datetime wrapper. Flash to ESP32-S3.
2.  **Compile App:** Run the CMake build for `app_clock`. Verify `clock.espapp` is generated and is **< 50KB** (proves no stdlib bloat).
3.  **Deploy App:** Copy `clock.espapp` to `/sdcard/apps/` and insert the SD card.
4.  **Observe Boot Sequence:**
    *   Kernel boots, mounts SD.
    *   Loader reads `clock.espapp`, verifies signature.
    *   PSRAM pool allocates ~10KB.
    *   Relocations applied in < 5ms.
    *   `app_main` executes.
5.  **Verify UI:**
    *   Screen clears.
    *   "00:00:00" appears at top in large font.
    *   Calendar widget renders at bottom.
6.  **Verify NTP Sync:**
    *   Connect WiFi. Wait 5 seconds.
    *   Time label should jump to current wall-clock time.
    *   Calendar should highlight today's date.
7.  **Verify Unload/Reload:**
    *   Trigger a kernel event to unload the clock app (e.g., simulate launcher switch).
    *   Verify `psram_pool_dump_stats()` shows **0 bytes used**.
    *   Reload the app. Verify it starts instantly without UI artifacts.

If this sequence completes successfully, **you have a fully functional, dynamically loading embedded OS**. 

---

## App Launcher 

The App Launcher is the central hub of ESP-AppOS. It must dynamically discover installed apps, render a touch-friendly grid, and instruct the kernel to swap the active application.
To achieve this without standard library bloat, we need to extend the Kernel API with directory iteration, app-launch requests, and flexible UI layout support.


### FS Directory Iteration (svc_fs.c)
Wrap ESP-IDF's POSIX directory functions, enforcing path sandboxing.

### Launch Request (lifecycle_manager.c)
The lifecycle manager needs a global state to track the next app to run.


static char g_pending_launch[32] = {0};

k_err_t svc_sys_request_launch(const char* app_name) {
    if (!app_name || strlen(app_name) >= sizeof(g_pending_launch)) return K_ERR_INVALID;
    strncpy(g_pending_launch, app_name, sizeof(g_pending_launch) - 1);
    g_pending_launch[sizeof(g_pending_launch) - 1] = '\0';
    ESP_LOGI("LIFECYCLE", "Pending launch requested: %s", g_pending_launch);
    return K_OK;
}

/* In the main kernel loop: */
void kernel_lifecycle_loop(void) {
    char current_app[32] = "launcher"; // Start with launcher
    
    while (1) {
        app_context_t* ctx = NULL;
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/apps/%s.espapp", current_app);
        
        load_result_t res = app_loader_load(path, &ctx);
        if (res == LOAD_OK) {
            app_sandbox_run(ctx); // Blocks until app exits or crashes
            app_loader_unload(ctx);
        } else {
            ESP_LOGE("LIFECYCLE", "Failed to load %s: %s", current_app, app_loader_error_str(res));
            vTaskDelay(pdMS_TO_TICKS(2000)); // Prevent bootloop
        }
        
        // Determine next app
        if (g_pending_launch[0] != '\0') {
            strncpy(current_app, g_pending_launch, sizeof(current_app));
            g_pending_launch[0] = '\0'; // Clear flag
        } else {
            strcpy(current_app, "launcher"); // Fallback to launcher
        }
    }
}


### Display Service Flex Layout Support
Update svc_display.c to support grid layouts via the string property API.


/* Add to svc_display_obj_set_prop */
else if (strcmp(prop, "layout") == 0) {
    if (strcmp(value, "flex_row_wrap") == 0) {
        lv_obj_set_layout(o, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    } else if (strcmp(value, "flex_col") == 0) {
        lv_obj_set_layout(o, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    }
}
else if (strcmp(prop, "pad_all") == 0) {
    lv_obj_set_style_pad_all(o, atoi(value), 0);
}
else if (strcmp(prop, "symbol") == 0) {
    lv_label_set_text(o, value); // LV_SYMBOL_* are just special UTF-8 strings
}


---

### The Launcher App (app_launcher.c)
This app scans /sdcard/apps/, parses .manifest.json files using a minimal custom parser (no cJSON dependency), and renders a grid of icons.

Manifest File Standard
To test the launcher, create manifest files on your SD card alongside the .espapp binaries.
/sdcard/apps/clock.manifest.json

```json
{
  "name": "Clock",
  "icon": "clock.bin",
  "version": "1.0.0",
  "abi_min": 1
}
```

/sdcard/apps/launcher.manifest.json

```json
{
  "name": "Home",
  "icon": "home.bin",
  "version": "1.0.0",
  "abi_min": 1
}
```

---

## Audio Player App

To validate the I2S ringbuffer pipeline, we will build an Audio Player App that streams raw PCM data from .wav files on the SD card. For this validation, we will bypass MP3 decoding (which requires bundling a heavy library like libhelix or minimp3 into the app) and focus strictly on the Filesystem → App Buffer → Kernel Ringbuffer → I2S DMA pipeline. This ensures the hardware audio path is bulletproof before adding CPU-intensive decoding layers in Phase 3.

### Critical Design: Non-Blocking Streaming

A common embedded mistake is blocking the UI thread (LVGL timer) while waiting for the audio buffer to drain or the SD card to read. This app uses a zero-timeout write strategy with a "pending chunk" state. If the kernel's audio ringbuffer is full, the app saves the chunk in RAM and retries on the next 20ms timer tick. The UI remains at 60FPS even if the SD card stalls.

### The Audio Player App (app_audio.c)
This app is entirely self-contained, uses zero standard library functions, and manages its own resources cleanly on exit.

### Testing the Pipeline End-to-End
To validate the hardware and software stack, follow this exact procedure:

**Step 1**: Generate Test Audio
Your kernel's I2S DMA is currently hardcoded to 44.1kHz, 16-bit, Stereo. You must generate a compatible test file.
Using ffmpeg on your PC:

```bash
ffmpeg -i your_song.mp3 -ar 44100 -ac 2 -sample_fmt s16 test_audio.wav
```

**Step 2**: Prepare SD Card
- Create a folder named media at the root of your SD card.
- Copy test_audio.wav into /media/.
- Ensure audio.espapp and audio.manifest.json are in /apps/.

**Step 3**: Validation Matrix
Execute these tests to verify the architecture:

| Test Scenario | Expected Behavior | What it Validates |
| :--- | :--- | :--- |
| **Boot & Navigate** | Launcher shows "Audio Player". Tap to load. | App loading, manifest parsing, UI rendering. |
| **File Discovery** | Lists `test_audio.wav` in the UI grid. | `fs.opendir` / `fs.readdir` sandbox routing. |
| **Initial Play** | Tap file → Tap Play. Audio starts instantly. | WAV header skip, I2S DMA startup, Ringbuffer feed. |
| **UI Responsiveness** | Tap buttons rapidly while audio plays. | **Crucial:** Zero-timeout writes prove LVGL task is never blocked by audio/SD. |
| **Pause / Resume** | Tap Pause. Audio stops. Tap Resume. Audio continues from exact same timestamp. | File handle persistence, timer recreation. |
| **Track Switching** | Tap a second file while first is playing. | Graceful teardown of I2S handles, `app_deinit` logic. |
| **SD Card Eject** | Physically remove SD card while playing. | Kernel Storage Manager triggers force-quit → `app_deinit` runs → No kernel crash. |
| **App Exit** | Long-press BACK to return to Launcher. | PSRAM pool shows 0 bytes used (no memory leaks). |

---

## Validation Checklist

| Test Step | Expected Behavior |
| :--- | :--- |
| **1. Clean Boot** | Kernel mounts SD → Loads `launcher.espapp` → Renders "ESP-AppOS" header and grid. |
| **2. Discovery** | Reads `clock.manifest.json` → Renders "Clock" button with `LV_SYMBOL_CLOCK`. |
| **3. Missing Manifest** | If `clock.manifest.json` is missing, the app is ignored (or shows raw filename if logic adjusted). |
| **4. Touch/Click** | Tapping "Clock" triggers `on_app_clicked` → Calls `request_launch("clock")` → Calls `request_exit()`. |
| **5. Transition** | Launcher unloads (PSRAM freed) → Kernel lifecycle loop reads `g_pending_launch` → Loads `clock.espapp`. |
| **6. Return Home** | (Future) Long-press BACK button triggers system gesture → Force-quits Clock → Kernel falls back to `launcher`. |
| **7. Memory Leak** | Launch/exit cycle 10 times. `psram_pool_dump_stats()` must show 0 used blocks between transitions. |

## Development challanges

App Compatibility Drift: If kernel API changes, old downloaded apps break. Implement API versioning in manifest and backward-compatible shim layers.
Security of Downloaded Apps: Unsigned binaries = malware risk. Implement ED25519 signature verification in bootloader/app-loader before execution.





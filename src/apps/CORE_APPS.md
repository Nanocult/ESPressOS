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






